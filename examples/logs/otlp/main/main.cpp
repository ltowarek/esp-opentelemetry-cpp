#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_log_otel.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"
#include <cstring>
#include "sdkconfig.h"
#include "esp_git_ref.hpp"
#include "esp_opentelemetry.hpp"

static const char *TAG = "logs-otlp-example";

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

// OTLP log records carry absolute timestamps, and the ESP32 has no clock of
// its own: until SNTP sets the system time, system_clock::now() returns
// seconds since boot, which reads as 1970. Loki and most other backends
// reject records that old, so time must be set before any record is emitted.
static void sync_time()
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000)) != ESP_ERR_TIMEOUT) {
            ESP_LOGI(TAG, "System time set");
            return;
        }
    }
    ESP_LOGW(TAG, "SNTP sync timed out; exported records will be dropped by "
                  "backends that reject 1970 timestamps");
}

extern "C" void app_main()
{
    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID: %s ...", CONFIG_WIFI_SSID);
    if (!wifi_connect()) {
        ESP_LOGE(TAG, "Failed to connect to Wi-Fi");
        return;
    }
    ESP_LOGI(TAG, "Wi-Fi connected");

    sync_time();

    // Every ESP_LOGx below this point is also exported. The calls above ran
    // before the provider existed and reached the console only - the API's
    // no-op logger silently drops records until a provider is installed.
    esp_opentelemetry_logs_setup({
        {"service.name", CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME},
        {"vcs.repository.url.full", CONFIG_ESP_OPENTELEMETRY_SERVICE_REPOSITORY},
        {"vcs.ref.head.revision", esp_opentelemetry::current_git_ref()},
    });

    ESP_LOGI(TAG, "Logs OTLP base URL: %s",
             CONFIG_ESP_OPENTELEMETRY_LOGS_OTLP_BASE_URL);

    for (unsigned tick = 0;; ++tick) {
        ESP_LOGI(TAG, "tick %u, uptime %llds", tick, esp_timer_get_time() / 1000000);
        if (tick % 5 == 4) {
            ESP_LOGW(TAG, "every fifth tick is a warning");
        }
        if (tick % 10 == 9) {
            ESP_LOGE(TAG, "every tenth tick is an error");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
