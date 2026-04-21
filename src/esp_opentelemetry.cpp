#include "esp_opentelemetry.hpp"

#include "sdkconfig.h"

#include "propagation.hpp"

#include "opentelemetry/trace/provider.h"

#if defined(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED)
#include "esp_http_client_transport.hpp"
#include "esp_log.h"

#include "opentelemetry/context/propagation/global_propagator.h"
#include "opentelemetry/exporters/otlp/otlp_http_exporter.h"
#include "opentelemetry/exporters/otlp/otlp_http_exporter_options.h"
#include "opentelemetry/sdk/resource/resource.h"
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

namespace otlp_api   = opentelemetry::exporter::otlp;
namespace sdk_trace  = opentelemetry::sdk::trace;
namespace sdk_res    = opentelemetry::sdk::resource;

static constexpr const char* TAG = "esp_opentelemetry";

static std::unique_ptr<sdk_trace::SpanExporter> MakeExporter(
    const std::string& endpoint) {
  otlp_api::OtlpHttpExporterOptions options;
  options.url          = endpoint + "/v1/traces";
  options.content_type = otlp_api::HttpRequestContentType::kJson;
  options.json_bytes_mapping = otlp_api::JsonBytesMappingKind::kHexId;
  options.use_json_name      = false;
  options.console_debug      = false;
  options.timeout            = std::chrono::seconds(10);
  options.compression        = "none";
  // HTTP transport is supplied by our esp_http_client-backed
  // HttpClientFactory (esp_http_client_transport.cpp),
  // linked in as the `opentelemetry_http_client_curl` target.
  return std::unique_ptr<sdk_trace::SpanExporter>(
      new otlp_api::OtlpHttpExporter(options));
}

#endif  // CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED

void esp_opentelemetry_setup(const char* service_name) {
  bool expected = false;
  if (!g_initialised.compare_exchange_strong(expected, true)) {
    return;
  }

#if defined(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED)
  const std::string endpoint = CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_ENDPOINT;
  if (endpoint.empty()) {
    ESP_LOGW(TAG, "ESP_OPENTELEMETRY endpoint is empty; tracing disabled.");
    return;
  }

  auto exporter = MakeExporter(endpoint);

  sdk_trace::BatchSpanProcessorOptions batch_options;
  batch_options.max_queue_size        = CONFIG_ESP_OPENTELEMETRY_BATCH_MAX_QUEUE_SIZE;
  batch_options.schedule_delay_millis =
      std::chrono::milliseconds(CONFIG_ESP_OPENTELEMETRY_BATCH_SCHEDULE_DELAY_MS);

  auto processor = std::unique_ptr<sdk_trace::SpanProcessor>(
      new sdk_trace::BatchSpanProcessor(std::move(exporter), batch_options));

  auto resource = sdk_res::Resource::Create(
      {{"service.name", service_name ? service_name : kTracerName}});

  auto provider = sdk_trace::TracerProviderFactory::Create(std::move(processor),
                                                            resource);
  trace_api::Provider::SetTracerProvider(
      nostd_api::shared_ptr<trace_api::TracerProvider>(provider.release()));

  context_api::propagation::GlobalTextMapPropagator::SetGlobalPropagator(
      nostd_api::shared_ptr<context_api::propagation::TextMapPropagator>(
          new trace_api::propagation::HttpTraceContext()));

  ESP_LOGI(TAG, "OpenTelemetry tracing enabled for %s -> %s",
           service_name ? service_name : kTracerName, endpoint.c_str());
#else
  (void)service_name;
#endif
}

opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer>
esp_opentelemetry_tracer() {
  auto provider = trace_api::Provider::GetTracerProvider();
  return provider->GetTracer(kTracerName, kTracerVersion);
}

void esp_opentelemetry_inject_traceparent(cJSON* obj) {
  esp_opentelemetry::inject_traceparent(obj);
}

opentelemetry::context::Context
esp_opentelemetry_extract_traceparent(const cJSON* obj) {
  return esp_opentelemetry::extract_traceparent(obj);
}
