#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_pthread.h"
#include "nvs_flash.h"
#include <cstring>
#include "sdkconfig.h"
#include "esp_opentelemetry.hpp"
#include "opentelemetry/trace/scope.h"
#include "opentelemetry/trace/span_startoptions.h"

static const char *TAG = "batch-example";

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

extern "C" void app_main()
{
    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID: %s ...", CONFIG_WIFI_SSID);
    if (!wifi_connect()) {
        ESP_LOGE(TAG, "Failed to connect to Wi-Fi");
        return;
    }
    ESP_LOGI(TAG, "Wi-Fi connected");

    // Route the BatchSpanProcessor background thread stack to PSRAM so the
    // ~32 KB allocation doesn't exhaust the 520 KB internal DRAM.
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

    ESP_LOGI(TAG, "OTLP endpoint : %s",
             CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_ENDPOINT);
    ESP_LOGI(TAG, "Batch flush interval: %d ms",
             CONFIG_ESP_OPENTELEMETRY_BATCH_SCHEDULE_DELAY_MS);

    // Root span — all child spans share this trace ID.
    auto root = tracer->StartSpan(
        "batch.demo",
        {{"example.name", "batch"},
         {"span.count", static_cast<int64_t>(5)}});
    auto root_scope = opentelemetry::trace::Scope(root);

    // Create several child spans to fill the batch queue and demonstrate
    // that the BatchSpanProcessor groups them into a single HTTP POST.
    ESP_LOGI(TAG, "Queueing child spans...");
    for (int i = 0; i < 5; i++) {
        char name[32];
        snprintf(name, sizeof(name), "batch.item.%d", i);

        opentelemetry::trace::StartSpanOptions opts;
        opts.parent = root->GetContext();

        auto span = tracer->StartSpan(
            name,
            {{"batch.index", static_cast<int64_t>(i)},
             {"batch.total", static_cast<int64_t>(5)}},
            opts);
        span->End();

        ESP_LOGI(TAG, "  queued: %s", name);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    root->End();

    // Sleep past the flush interval so the HTTP POST completes
    // before the log message below confirms success.
    const int flush_wait_ms = CONFIG_ESP_OPENTELEMETRY_BATCH_SCHEDULE_DELAY_MS + 5000;
    ESP_LOGI(TAG, "All spans queued — waiting %d ms for BatchSpanProcessor to flush...",
             flush_wait_ms);
    vTaskDelay(pdMS_TO_TICKS(flush_wait_ms));

    ESP_LOGI(TAG, "Done. Check your OTLP collector for service '%s'.",
             CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME);
}
