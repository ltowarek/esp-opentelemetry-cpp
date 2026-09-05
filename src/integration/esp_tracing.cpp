#include "esp_tracing.hpp"

#include "sdkconfig.h"

#include "opentelemetry/trace/provider.h"

#if defined(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED)
#include "esp_export_thread.hpp"
#include "esp_log.h"

#include "opentelemetry/context/propagation/global_propagator.h"
#if defined(CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_HTTP)
#include "esp_otlp_http_exporters.hpp"
#endif
#include "opentelemetry/sdk/trace/batch_span_processor.h"
#include "opentelemetry/sdk/trace/batch_span_processor_options.h"
#include "opentelemetry/sdk/trace/tracer_provider.h"
#include "opentelemetry/sdk/trace/tracer_provider_factory.h"
#include "opentelemetry/trace/propagation/http_trace_context.h"

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#endif

#include <atomic>

namespace trace_api    = opentelemetry::trace;
namespace nostd_api    = opentelemetry::nostd;
namespace context_api  = opentelemetry::context;

namespace {

std::atomic<bool> g_initialised{false};

constexpr const char* kTracerName    = "esp-opentelemetry-cpp";
constexpr const char* kTracerVersion = "1.0.0";

}  // namespace

#if defined(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED)

namespace sdk_trace  = opentelemetry::sdk::trace;
namespace sdk_res    = opentelemetry::sdk::resource;

static constexpr const char* TAG = "esp_opentelemetry";

#endif  // CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED

void esp_opentelemetry_tracing_setup(
    std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> exporter,
    opentelemetry::sdk::resource::ResourceAttributes resource_attrs) {
#if defined(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED)
  if (exporter == nullptr) {
    ESP_LOGW(TAG, "no span exporter supplied; tracing disabled.");
    return;
  }

  sdk_trace::BatchSpanProcessorOptions batch_options;
  batch_options.max_queue_size        = CONFIG_ESP_OPENTELEMETRY_BATCH_MAX_QUEUE_SIZE;
  batch_options.schedule_delay_millis =
      std::chrono::milliseconds(CONFIG_ESP_OPENTELEMETRY_BATCH_SCHEDULE_DELAY_MS);

  std::unique_ptr<sdk_trace::SpanProcessor> processor;
  {
    // The BatchSpanProcessor constructor spawns its export pthread.
    esp_opentelemetry::ScopedExportThreadConfig export_thread_cfg;
    processor = std::unique_ptr<sdk_trace::SpanProcessor>(
        new sdk_trace::BatchSpanProcessor(std::move(exporter), batch_options));
  }

  esp_opentelemetry_tracing_setup(std::move(processor), std::move(resource_attrs));
#else
  (void)exporter;
  (void)resource_attrs;
#endif
}

void esp_opentelemetry_tracing_setup(
    std::unique_ptr<opentelemetry::sdk::trace::SpanProcessor> processor,
    opentelemetry::sdk::resource::ResourceAttributes resource_attrs) {
#if defined(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED)
  if (processor == nullptr) {
    ESP_LOGW(TAG, "no span processor supplied; tracing disabled.");
    return;
  }

  bool expected = false;
  if (!g_initialised.compare_exchange_strong(expected, true)) {
    return;
  }

  auto resource = sdk_res::Resource::Create(resource_attrs);

  auto provider = sdk_trace::TracerProviderFactory::Create(std::move(processor),
                                                            resource);
  trace_api::Provider::SetTracerProvider(
      nostd_api::shared_ptr<trace_api::TracerProvider>(provider.release()));

  context_api::propagation::GlobalTextMapPropagator::SetGlobalPropagator(
      nostd_api::shared_ptr<context_api::propagation::TextMapPropagator>(
          new trace_api::propagation::HttpTraceContext()));

  ESP_LOGI(TAG, "OpenTelemetry tracing enabled");
#else
  (void)processor;
  (void)resource_attrs;
#endif
}

void esp_opentelemetry_tracing_setup(
    opentelemetry::sdk::resource::ResourceAttributes resource_attrs) {
#if defined(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED) && \
    defined(CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_HTTP)
  const std::string endpoint = CONFIG_ESP_OPENTELEMETRY_TRACING_OTLP_BASE_URL;
  if (endpoint.empty()) {
    ESP_LOGW(TAG, "tracing base URL is empty; tracing disabled.");
    return;
  }
  esp_opentelemetry_tracing_setup(
      esp_opentelemetry::MakeOtlpHttpSpanExporter(endpoint), resource_attrs);
#else
  (void)resource_attrs;
#endif
}

opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer>
esp_opentelemetry_tracer() {
  auto provider = trace_api::Provider::GetTracerProvider();
  return provider->GetTracer(kTracerName, kTracerVersion);
}
