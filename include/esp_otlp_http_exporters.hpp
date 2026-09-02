// OTLP/HTTP exporters bound to esp_http_client.
//
// opentelemetry-cpp's own OtlpHttp*ExporterFactory::Create() builds a libcurl
// client, which does not cross-compile to Xtensa. These factories are the same
// exporters with the ESP HTTP client supplied instead, so application code
// selects an exporter exactly as it would upstream:
//
//   auto exporter  = esp_opentelemetry::MakeOtlpHttpSpanExporter(url);
//   auto processor = BatchSpanProcessorFactory::Create(std::move(exporter), opts);
//   auto provider  = TracerProviderFactory::Create(std::move(processor), resource);
//
// Requires CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_HTTP, and each signal's own
// ..._ENABLED option for that signal's factory.

#pragma once

#include "sdkconfig.h"

#if defined(CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_HTTP)

#include "esp_profiles_exporter.hpp"

#include <memory>
#include <string>

#if defined(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED)
#include "opentelemetry/sdk/trace/exporter.h"
#endif
#if defined(CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED)
#include "opentelemetry/sdk/logs/exporter.h"
#endif
#if defined(CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED)
#include "opentelemetry/sdk/metrics/push_metric_exporter.h"
#endif

namespace esp_opentelemetry {

#if defined(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED)
// base_url is the collector root; "/v1/traces" is appended.
std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> MakeOtlpHttpSpanExporter(
    const std::string& base_url);
#endif

#if defined(CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED)
// base_url is the collector root; "/v1/logs" is appended.
std::unique_ptr<opentelemetry::sdk::logs::LogRecordExporter>
MakeOtlpHttpLogRecordExporter(const std::string& base_url);
#endif

#if defined(CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED)
// base_url is the collector root; "/v1/metrics" is appended.
std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter>
MakeOtlpHttpMetricExporter(const std::string& base_url);
#endif

// base_url is the symbolizer root; "/v1development/profiles" is appended. Not
// gated on the profiling signal: this is a transport for a JSON document, not
// a hook into the profiler.
std::unique_ptr<ProfilesExporter> MakeOtlpHttpProfilesExporter(
    const std::string& base_url);

}  // namespace esp_opentelemetry

#endif  // CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_HTTP
