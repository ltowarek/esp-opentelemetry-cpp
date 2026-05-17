#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_pthread.h"
#include "nvs_flash.h"
#include <cstring>
#include <string>
#include "sdkconfig.h"
#include "esp_opentelemetry.hpp"
#include "opentelemetry/context/propagation/global_propagator.h"
#include "opentelemetry/context/propagation/text_map_propagator.h"
#include "opentelemetry/context/runtime_context.h"
#include "opentelemetry/trace/scope.h"

static const char *TAG = "propagation-example";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAXIMUM_RETRY 5

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_connect()
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {};
    strcpy((char *)wifi_config.sta.ssid, CONFIG_WIFI_SSID);
    strcpy((char *)wifi_config.sta.password, CONFIG_WIFI_PASSWORD);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

    return (bits & WIFI_CONNECTED_BIT) != 0;
}

// TextMapCarrier backed by esp_http_client so the propagator can inject
// the traceparent/tracestate headers directly into the pending request.
class EspHttpCarrier
    : public opentelemetry::context::propagation::TextMapCarrier {
public:
    explicit EspHttpCarrier(esp_http_client_handle_t client) : client_(client) {}

    opentelemetry::nostd::string_view Get(
        opentelemetry::nostd::string_view key) const noexcept override {
        // Read-back of injected headers is not needed for this example.
        return {};
    }

    void Set(opentelemetry::nostd::string_view key,
             opentelemetry::nostd::string_view value) noexcept override {
        esp_http_client_set_header(client_,
            std::string(key.data(), key.size()).c_str(),
            std::string(value.data(), value.size()).c_str());
        ESP_LOGI(TAG, "  inject: %.*s: %.*s",
                 (int)key.size(), key.data(),
                 (int)value.size(), value.data());
    }

private:
    esp_http_client_handle_t client_;
};

extern "C" void app_main()
{
    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID: %s ...", CONFIG_WIFI_SSID);
    if (!wifi_connect()) {
        ESP_LOGE(TAG, "Failed to connect to Wi-Fi");
        return;
    }
    ESP_LOGI(TAG, "Wi-Fi connected");

    // Route the BatchSpanProcessor background thread stack to PSRAM.
    {
        esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
        cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        esp_pthread_set_cfg(&cfg);
    }

    esp_opentelemetry_setup(CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME);

    {
        esp_pthread_cfg_t default_cfg = esp_pthread_get_default_config();
        esp_pthread_set_cfg(&default_cfg);
    }

    auto tracer = esp_opentelemetry_tracer();

    // Start a span so the active context carries a valid trace ID.
    auto span  = tracer->StartSpan("outgoing.request");
    auto scope = opentelemetry::trace::Scope(span);

    // Create an HTTP request and inject W3C TraceContext headers into it.
    esp_http_client_config_t cfg = {};
    cfg.url = CONFIG_TARGET_URL;
    cfg.method = HTTP_METHOD_GET;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    auto propagator =
        opentelemetry::context::propagation::GlobalTextMapPropagator::GetGlobalPropagator();
    auto ctx = opentelemetry::context::RuntimeContext::GetCurrent();
    EspHttpCarrier carrier(client);
    propagator->Inject(carrier, ctx);

    ESP_LOGI(TAG, "Sending request to %s with trace context headers...",
             CONFIG_TARGET_URL);
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Response status: %d",
                 esp_http_client_get_status_code(client));
    } else {
        ESP_LOGW(TAG, "Request failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);

    span->End();

    ESP_LOGI(TAG, "Done. Check your server logs for the traceparent header.");
}
