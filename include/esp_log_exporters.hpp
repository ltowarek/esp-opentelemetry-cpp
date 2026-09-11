// Exporters that print every signal through ESP-IDF's esp_log, so spans, log
// records, metric data points and profiles read like any other component's
// console output: tagged, level-filtered, and shown or hidden with
// idf.py monitor --print-filter / esp_log_level_set.
//
// Usage:
//
//   esp_opentelemetry_tracing_setup(
//       esp_opentelemetry::MakeEspLogSpanExporter(), resource);
//
// One factory per signal, each returning the SDK's own exporter interface (or
// this component's ProfilesExporter, which stands in for the interface the
// SDK does not have). Pass the result to the matching
// esp_opentelemetry_*_setup() call, or to a provider built by hand.
//
// See esp_logs.hpp for the logs setup call and esp_log_otel.h for the ESP log
// bridge, which carries data the opposite way: a plain ESP_LOGx call site
// becomes a log record there, while here a record (or span, metric point,
// profile) becomes an ESP_LOGx line. Enabling both at once double-prints a
// bridged line — once as the plain console line the bridge always writes,
// and again as this exporter's own log-record line.
//
// Requires CONFIG_ESP_OPENTELEMETRY_EXPORTER_ESP_LOG. Each signal's factory is
// compiled only when that signal is enabled; MakeEspLogProfilesExporter() is
// not, being a transport for a JSON document rather than a hook into the
// profiler.

#pragma once

#include "sdkconfig.h"

#if defined(CONFIG_ESP_OPENTELEMETRY_EXPORTER_ESP_LOG)

#include "esp_profiles_exporter.hpp"

#include <memory>

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
// Prints under the "otel.span" tag: INFO, or WARN when the span's status is
// Error. Each event and link prints on its own following line carrying the
// span's span_id.
std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> MakeEspLogSpanExporter();
#endif  // CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED

#if defined(CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED)
// Prints under the "otel.log" tag, at the ESP-IDF level matching the record's
// severity (TRACE->V, DEBUG->D, INFO->I, WARN->W, ERROR/FATAL->E). A record
// carrying a log.tag attribute (set by the ESP log bridge) is also filtered
// by that tag's own runtime level.
std::unique_ptr<opentelemetry::sdk::logs::LogRecordExporter> MakeEspLogLogRecordExporter();
#endif  // CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED

#if defined(CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED)
// Prints one INFO line per data point under the "otel.metric" tag, repeating
// the instrument's fields, resource and scope on every point's line.
std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> MakeEspLogMetricExporter();
#endif  // CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED

// Prints the raw JSON document on one INFO line under the "otel.profile" tag,
// with no begin/end markers.
std::unique_ptr<ProfilesExporter> MakeEspLogProfilesExporter();

}  // namespace esp_opentelemetry

#endif  // CONFIG_ESP_OPENTELEMETRY_EXPORTER_ESP_LOG
