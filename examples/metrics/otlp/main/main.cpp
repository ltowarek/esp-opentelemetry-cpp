#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include <cstring>
#include "sdkconfig.h"
#include "esp_opentelemetry.hpp"
#include "opentelemetry/metrics/provider.h"

static const char *TAG = "metrics-otlp-example";

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

static void observe_uptime(opentelemetry::metrics::ObserverResult obs, void *)
{
    observe_int64(obs, esp_timer_get_time() / 1000000);
}

extern "C" void app_main()
{
    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID: %s ...", CONFIG_WIFI_SSID);
    if (!wifi_connect()) {
        ESP_LOGE(TAG, "Failed to connect to Wi-Fi");
        return;
    }
    ESP_LOGI(TAG, "Wi-Fi connected");

    esp_opentelemetry_metrics_setup();

    ESP_LOGI(TAG, "Metrics OTLP base URL: %s",
             CONFIG_ESP_OPENTELEMETRY_METRICS_OTLP_BASE_URL);

    auto meter = opentelemetry::metrics::Provider::GetMeterProvider()->GetMeter(
        CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME, "1.0.0");

    // Observable gauge: collected by the PeriodicExportingMetricReader every
    // CONFIG_ESP_OPENTELEMETRY_METRICS_EXPORT_INTERVAL_MS and exported via
    // OTLP/HTTP JSON.
    auto uptime = meter->CreateInt64ObservableGauge(
        "example.uptime", "Seconds since boot", "s");
    uptime->AddCallback(observe_uptime, nullptr);

    // Counter incremented on the app side each second.
    auto ticks = meter->CreateUInt64Counter("example.ticks", "Loop iterations");

    for (;;) {
        ticks->Add(1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
