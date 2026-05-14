#include "esp_opentelemetry.hpp"
#include "opentelemetry/exporters/ostream/span_exporter_factory.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/sdk/trace/simple_processor_factory.h"
#include "opentelemetry/sdk/trace/tracer_provider_factory.h"
#include "opentelemetry/trace/provider.h"

extern "C" void app_main()
{
    auto exporter  = opentelemetry::exporter::trace::OStreamSpanExporterFactory::Create();
    auto processor = opentelemetry::sdk::trace::SimpleSpanProcessorFactory::Create(std::move(exporter));
    auto resource  = opentelemetry::sdk::resource::Resource::Create({{"service.name", "ostream-example"}});
    auto provider  = opentelemetry::sdk::trace::TracerProviderFactory::Create(std::move(processor), resource);
    opentelemetry::trace::Provider::SetTracerProvider(
        opentelemetry::nostd::shared_ptr<opentelemetry::trace::TracerProvider>(provider.release()));

    auto tracer = esp_opentelemetry_tracer();
    auto span   = tracer->StartSpan("app_main");
    span->SetAttribute("example.type", "ostream");
    span->End();
}
