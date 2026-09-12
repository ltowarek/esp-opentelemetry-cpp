#include "esp_log_exporters.hpp"
#include "esp_opentelemetry.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "opentelemetry/trace/scope.h"

#include <cstdint>

static const char *TAG = "traces-example";

extern "C" void app_main()
{
    const opentelemetry::sdk::resource::ResourceAttributes resource{
        {"service.name", CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME}};

    // The application picks the exporter, as it does upstream; the setup call
    // only wires it into a processor and provider.
    esp_opentelemetry_tracing_setup(
        esp_opentelemetry::MakeEspLogSpanExporter(), resource);

    auto tracer = esp_opentelemetry_tracer();

    for (int iteration = 0; iteration < 5; ++iteration) {
        auto parent = tracer->StartSpan("work.iteration");
        parent->SetAttribute("example.type", "traces");
        parent->SetAttribute("iteration", static_cast<int64_t>(iteration));
        {
            opentelemetry::trace::Scope scope(parent);
            auto child = tracer->StartSpan("work.step");
            child->AddEvent("work.step.checkpoint");
            vTaskDelay(pdMS_TO_TICKS(50));
            child->End();
        }
        parent->End();

        ESP_LOGI(TAG, "iteration %d", iteration);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Shows the Error-status field the healthy loop above never sets.
    auto failed = tracer->StartSpan("work.failed");
    failed->SetStatus(opentelemetry::trace::StatusCode::kError, "simulated failure");
    failed->End();

    ESP_LOGI(TAG, "done");
}
