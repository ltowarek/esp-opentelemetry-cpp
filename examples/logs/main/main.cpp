#include "esp_opentelemetry.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "opentelemetry/exporters/ostream/log_record_exporter_factory.h"

static const char *TAG = "logs-example";

extern "C" void app_main()
{
    const opentelemetry::sdk::resource::ResourceAttributes resource{
        {"service.name", CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME}};

    esp_opentelemetry_logs_setup(
        opentelemetry::exporter::logs::OStreamLogRecordExporterFactory::Create(), resource);

    auto logger = esp_opentelemetry_logger();

    for (int iteration = 0; iteration < 5; ++iteration) {
        logger->Info("iteration complete");

        ESP_LOGI(TAG, "iteration %d", iteration);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "done");
}
