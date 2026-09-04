#include "esp_opentelemetry.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "opentelemetry/exporters/ostream/metric_exporter_factory.h"
#include "opentelemetry/metrics/provider.h"

static const char *TAG = "metrics-example";

extern "C" void app_main()
{
    const opentelemetry::sdk::resource::ResourceAttributes resource{
        {"service.name", CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME}};

    esp_opentelemetry_metrics_setup(
        opentelemetry::exporter::metrics::OStreamMetricExporterFactory::Create(), resource);

    auto meter   = opentelemetry::metrics::Provider::GetMeterProvider()->GetMeter("metrics-example");
    auto counter = meter->CreateUInt64Counter("iterations", "Loop iterations", "1");

    for (int iteration = 0; iteration < 5; ++iteration) {
        counter->Add(1);

        ESP_LOGI(TAG, "iteration %d", iteration);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "done");
}
