#include "esp_jtag_exporters.hpp"
#include "esp_opentelemetry.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "opentelemetry/metrics/provider.h"
#include "opentelemetry/sdk/trace/simple_processor_factory.h"
#include "opentelemetry/trace/scope.h"

#include <cstdint>
#include <memory>

static const char *TAG = "jtag-example";

extern "C" void app_main()
{
    const opentelemetry::sdk::resource::ResourceAttributes resource{
        {"service.name", CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME}};
    esp_opentelemetry_tracing_setup(
        opentelemetry::sdk::trace::SimpleSpanProcessorFactory::Create(
            std::make_unique<esp_opentelemetry::JtagSpanExporter>()),
        resource);
    esp_opentelemetry_logs_setup(
        std::make_unique<esp_opentelemetry::JtagLogRecordExporter>(), resource);
    esp_opentelemetry_metrics_setup(
        std::make_unique<esp_opentelemetry::JtagMetricExporter>(), resource);
    esp_opentelemetry_profiling_setup(
        std::make_unique<esp_opentelemetry::JtagProfilesExporter>());

    auto tracer  = esp_opentelemetry_tracer();
    auto logger  = esp_opentelemetry_logger();
    auto meter   = opentelemetry::metrics::Provider::GetMeterProvider()->GetMeter("jtag-example");
    auto counter = meter->CreateUInt64Counter("iterations", "Loop iterations", "1");

    // Emitted on a loop so signals keep arriving after OpenOCD attaches. Until
    // it does, app-trace runs in post-mortem mode and overwrites them.
    for (int iteration = 0;; ++iteration) {
        auto parent = tracer->StartSpan("work.iteration");
        parent->SetAttribute("example.type", "jtag");
        parent->SetAttribute("iteration", static_cast<int64_t>(iteration));
        {
            opentelemetry::trace::Scope scope(parent);
            auto child = tracer->StartSpan("work.step");
            vTaskDelay(pdMS_TO_TICKS(100));
            child->End();
        }
        parent->End();

        counter->Add(1);
        logger->Info("iteration complete");

        ESP_LOGI(TAG, "emitted iteration %d", iteration);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
