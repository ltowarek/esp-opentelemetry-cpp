#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include <cstring>
#include "sdkconfig.h"
#include "esp_opentelemetry.hpp"
#include "opentelemetry/trace/scope.h"

static const char *TAG = "profiling-otlp";

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

// A recognizable hot function: with the sampler running, this dominates the
// flame graph. noinline so the symbolized frame carries this exact name.
static uint64_t g_sink;
static void __attribute__((noinline)) burn_cpu()
{
    uint64_t acc = 0;
    for (int i = 0; i < 200000; i = i + 1) {
        acc += static_cast<uint64_t>(i) * i;
    }
    g_sink = g_sink + acc;
}

extern "C" void app_main()
{
    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID: %s ...", CONFIG_WIFI_SSID);
    if (!wifi_connect()) {
        ESP_LOGE(TAG, "Failed to connect to Wi-Fi");
        return;
    }
    ESP_LOGI(TAG, "Wi-Fi connected");

    esp_opentelemetry_tracing_setup(CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME);
    esp_opentelemetry_profiling_setup();

    ESP_LOGI(TAG, "Profiles OTLP base URL: %s",
             CONFIG_ESP_OPENTELEMETRY_PROFILES_OTLP_BASE_URL);

    auto tracer = esp_opentelemetry_tracer();

    // Forever: one span per iteration wrapping the hot loop, so profile
    // samples carry the span link (span profiles), followed by idle time so
    // the flame graph shows both burn_cpu() and the idle task.
    for (uint32_t i = 0;; ++i) {
        {
            auto span = tracer->StartSpan("example.burn");
            auto scope = opentelemetry::trace::Scope(span);
            burn_cpu();
            span->End();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        if (i % 50 == 0) {
            ESP_LOGI(TAG, "samples captured so far: %lu",
                     static_cast<unsigned long>(esp_opentelemetry_profiling_samples()));
        }
    }
}
