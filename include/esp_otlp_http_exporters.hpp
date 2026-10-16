// OTLP/HTTP exporters bound to esp_http_client.
//
// These factories build the same OtlpHttp*Exporter classes upstream provides,
// with the ESP HTTP client supplied instead of the default one, so
// application code selects an exporter exactly as it would upstream:
//
//   auto exporter  = esp_opentelemetry::MakeOtlpHttpSpanExporter(url);
//   auto processor = BatchSpanProcessorFactory::Create(std::move(exporter), opts);
//   auto provider  = TracerProviderFactory::Create(std::move(processor), resource);
//
// Requires CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_HTTP. Every factory and
// every signal's ..._OTLP_BASE_URL option exist whenever this family is
// compiled in; a factory whose signal is disabled returns nullptr, which the
// signal's setup call treats as the signal off.

#pragma once

#include "sdkconfig.h"

#if defined(CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_HTTP)

#include "esp_profiles_exporter.hpp"

#include <memory>
#include <string>

#include "opentelemetry/sdk/logs/exporter.h"
#include "opentelemetry/sdk/metrics/push_metric_exporter.h"
#include "opentelemetry/sdk/trace/exporter.h"

namespace esp_opentelemetry {

// base_url is the collector root; "/v1/traces" is appended.
std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> MakeOtlpHttpSpanExporter(
    const std::string& base_url);

// base_url is the collector root; "/v1/logs" is appended.
std::unique_ptr<opentelemetry::sdk::logs::LogRecordExporter>
MakeOtlpHttpLogRecordExporter(const std::string& base_url);

// base_url is the collector root; "/v1/metrics" is appended.
std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter>
MakeOtlpHttpMetricExporter(const std::string& base_url);

// base_url is the symbolizer root; "/v1development/profiles" is appended. Not
// gated on the profiling signal: this is a transport for a JSON document, not
// a hook into the profiler.
std::unique_ptr<ProfilesExporter> MakeOtlpHttpProfilesExporter(
    const std::string& base_url);

}  // namespace esp_opentelemetry

#endif  // CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_HTTP
