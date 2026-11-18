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
// every signal's ..._OTLP_ENDPOINT option exist whenever this family is
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

// url is the full endpoint, e.g. http://host:4318/v1/traces.
std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> MakeOtlpHttpSpanExporter(
    const std::string& url);

// url is the full endpoint, e.g. http://host:4318/v1/logs.
std::unique_ptr<opentelemetry::sdk::logs::LogRecordExporter>
MakeOtlpHttpLogRecordExporter(const std::string& url);

// url is the full endpoint, e.g. http://host:4318/v1/metrics.
std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter>
MakeOtlpHttpMetricExporter(const std::string& url);

// url is the full symbolizer endpoint, e.g.
// http://host:4319/v1development/profiles.
std::unique_ptr<ProfilesExporter> MakeOtlpHttpProfilesExporter(
    const std::string& url);

}  // namespace esp_opentelemetry

#endif  // CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_HTTP
