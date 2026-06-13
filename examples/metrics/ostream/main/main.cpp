#include "opentelemetry/exporters/ostream/metric_exporter_factory.h"
#include "opentelemetry/metrics/observer_result.h"
#include "opentelemetry/metrics/provider.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h"
#include "opentelemetry/sdk/metrics/meter_context_factory.h"
#include "opentelemetry/sdk/metrics/meter_provider_factory.h"
#include "opentelemetry/sdk/metrics/provider.h"
#include "opentelemetry/sdk/metrics/view/view_registry_factory.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <chrono>
#include <memory>

namespace metrics_api = opentelemetry::metrics;

static void observe_double(opentelemetry::metrics::ObserverResult &obs, double value)
{
    opentelemetry::nostd::get<
        opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<double>>>(obs)
        ->Observe(value);
}

extern "C" void app_main()
{
    auto exporter = opentelemetry::exporter::metrics::OStreamMetricExporterFactory::Create();

    opentelemetry::sdk::metrics::PeriodicExportingMetricReaderOptions reader_opts;
    reader_opts.export_interval_millis = std::chrono::milliseconds(1000);
    reader_opts.export_timeout_millis  = std::chrono::milliseconds(500);
    auto reader = opentelemetry::sdk::metrics::PeriodicExportingMetricReaderFactory::Create(
        std::move(exporter), reader_opts);

    auto resource = opentelemetry::sdk::resource::Resource::Create(
        {{"service.name", CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME}});
    auto context = opentelemetry::sdk::metrics::MeterContextFactory::Create(
        opentelemetry::sdk::metrics::ViewRegistryFactory::Create(), resource);
    context->AddMetricReader(std::move(reader));

    auto sdk_provider =
        opentelemetry::sdk::metrics::MeterProviderFactory::Create(std::move(context));
    std::shared_ptr<metrics_api::MeterProvider> api_provider = std::move(sdk_provider);
    opentelemetry::sdk::metrics::Provider::SetMeterProvider(api_provider);

    auto meter = metrics_api::Provider::GetMeterProvider()->GetMeter(
        CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME, "1.0.0");

    auto counter = meter->CreateDoubleCounter(
        "example.requests", "Requests processed", "{request}");

    auto gauge = meter->CreateDoubleObservableGauge(
        "example.temperature", "Chip temperature", "Cel");
    gauge->AddCallback([](opentelemetry::metrics::ObserverResult obs, void *) {
        observe_double(obs, 42.0);
    }, nullptr);

    for (int i = 0; i < 5; ++i) {
        counter->Add(1.0);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
