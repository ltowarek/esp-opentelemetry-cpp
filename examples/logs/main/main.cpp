#include "esp_log_exporters.hpp"
#include "esp_log_otel_emit.h"
#include "esp_opentelemetry.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "logs-example";
// Tags distinct from TAG, so raising one's runtime level doesn't touch this
// file's own ESP_LOGI(TAG, ...) calls below.
static const char *BRIDGED_TAG = "logs-example-bridged";
static const char *BRIDGED_TAG_VISIBLE = "logs-example-bridged-visible";

extern "C" void app_main()
{
    const opentelemetry::sdk::resource::ResourceAttributes resource{
        {"service.name", CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME}};

    esp_opentelemetry_logs_setup(
        esp_opentelemetry::MakeEspLogLogRecordExporter(), resource);

    auto logger = esp_opentelemetry_logger();

    // Filtered by level alone: DEBUG is below the default INFO level, so
    // esp_log itself would not print it, and neither does the exporter.
    logger->Debug("filtered by level");

    // Anchors the tag-filtering check below: same bridge call, but its tag's
    // level is never raised, so the record must reach the exporter and print.
    // Without this, an absent "filtered by tag" line could just as easily
    // mean the bridge path was never exercised at all.
    esp_opentelemetry_log_and_emit(ESP_LOG_INFO, BRIDGED_TAG_VISIBLE, __FILE__, __LINE__, __func__,
                                   "bridged and visible");

    // Filtered by its original tag: esp_opentelemetry_log_and_emit() is the
    // bridge esp_log_otel.h's wrappers expand to (called directly here, so
    // this file's own ESP_LOGx calls keep their normal meaning). The record
    // reaches the exporter regardless of the tag's runtime level, but the
    // exporter must still skip it once that level hides it.
    esp_log_level_set(BRIDGED_TAG, ESP_LOG_WARN);
    esp_opentelemetry_log_and_emit(ESP_LOG_INFO, BRIDGED_TAG, __FILE__, __LINE__, __func__,
                                   "filtered by tag");

    for (int iteration = 0; iteration < 5; ++iteration) {
        logger->Info("iteration complete");

        ESP_LOGI(TAG, "iteration %d", iteration);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "done");
}
