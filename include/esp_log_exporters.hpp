// Exporters that print every signal through ESP-IDF's esp_log, so spans, log
// records, metric data points and profiles show up tagged and
// level-filterable like any other component's console output:
// idf.py monitor --print-filter / esp_log_level_set work on them.
//
// Usage:
//
//   esp_opentelemetry_tracing_setup(
//       esp_opentelemetry::MakeEspLogSpanExporter(), resource);
//
// MakeEspLogSpanExporter()/MakeEspLogLogRecordExporter()/MakeEspLogMetricExporter()
// delegate to the SDK's own Ostream exporter for that signal, with a custom
// std::streambuf in place of std::cout that routes each physical output line
// to one ESP_LOGI call under the signal's tag. The block layout (one span/log
// record/metric point prints as several lines - the SDK's own field order and
// padding) is unchanged; only the sink differs. Every line prints at INFO:
// the Ostream exporters have no per-item level of their own to carry through
// (no WARN-on-Error for spans, no severity-to-level mapping for log records),
// and esp_log's own tag/level gate is checked only once formatting is already
// done, not before.
//
// Profiles have no SDK Ostream exporter to delegate to, so
// MakeEspLogProfilesExporter() keeps the component's own single-line,
// no-markers JSON passthrough.
//
// See esp_logs.hpp for the logs setup call and esp_log_otel.h for the ESP log
// bridge, which carries data the opposite way: a plain ESP_LOGx call site
// becomes a log record there, while here a record (or span, metric point,
// profile) becomes ESP_LOGx lines. Enabling both at once double-prints a
// bridged line - once as the plain console line the bridge always writes,
// and again as this exporter's own lines for the resulting record.
//
// Requires CONFIG_ESP_OPENTELEMETRY_EXPORTER_ESP_LOG. Every factory exists
// whenever this family is compiled in; one whose signal is disabled returns
// nullptr, which the signal's setup call treats as the signal off.

#pragma once

#include "sdkconfig.h"

#if defined(CONFIG_ESP_OPENTELEMETRY_EXPORTER_ESP_LOG)

#include "esp_profiles_exporter.hpp"

#include <memory>

#include "opentelemetry/sdk/logs/exporter.h"
#include "opentelemetry/sdk/metrics/push_metric_exporter.h"
#include "opentelemetry/sdk/trace/exporter.h"

namespace esp_opentelemetry {

// Prints under the "otel.span" tag, at INFO.
std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> MakeEspLogSpanExporter();

// Prints under the "otel.log" tag, at INFO.
std::unique_ptr<opentelemetry::sdk::logs::LogRecordExporter> MakeEspLogLogRecordExporter();

// Prints under the "otel.metric" tag, at INFO.
std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> MakeEspLogMetricExporter();

// Prints the raw JSON document on one INFO line under the "otel.profile" tag,
// with no begin/end markers.
std::unique_ptr<ProfilesExporter> MakeEspLogProfilesExporter();

}  // namespace esp_opentelemetry

#endif  // CONFIG_ESP_OPENTELEMETRY_EXPORTER_ESP_LOG
