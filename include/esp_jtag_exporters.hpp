// OTLP exporters that write over the JTAG app-trace channel.
//
// Signals are serialised with the same OTLP/JSON encoding the OTLP/HTTP
// exporters send — one request document per line, newline-delimited — and
// written to ESP-IDF's app-trace channel. A host-side reader —
// `openocd ... -c "esp apptrace start tcp://..."` feeding Vector —
// forwards those lines to an OTLP/HTTP collector, so nothing on the host has
// to understand telemetry.
//
// Usage:
//
//   esp_opentelemetry_tracing_setup(
//       esp_opentelemetry::MakeJtagSpanExporter(), resource);
//
// One factory per signal, each returning the SDK's own exporter interface.
// All four share the single app-trace channel, one whole document at a time,
// so concurrent signals cannot interleave into an unparseable line. Every
// factory exists whenever this family is compiled in; one whose signal is
// disabled returns nullptr, which the signal's setup call treats as the
// signal off.
//
// Pass the result to the matching esp_opentelemetry_*_setup() call, or to a
// provider you build yourself - the application chooses its exporter, as it
// does upstream.
//
// A dropped document is logged but still reported to the processor as a
// successful export: OtlpFileAppender::Export returns void, so OtlpFileClient
// has no failure to pass on. Signal loss is visible in the device log and in
// what reaches the collector, not in the SDK's export result.
//
// Requires CONFIG_ESP_OPENTELEMETRY_EXPORTER_JTAG, which in turn
// requires the app-trace transport (CONFIG_ESP_TRACE_TRANSPORT_APPTRACE) with
// a destination that compiles the JTAG interface (CONFIG_APPTRACE_DEST_JTAG or
// CONFIG_APPTRACE_DEST_ALL). See examples/jtag.

#pragma once

#include "sdkconfig.h"

#if defined(CONFIG_ESP_OPENTELEMETRY_EXPORTER_JTAG)

#include "esp_profiles_exporter.hpp"

#include <memory>

#include "opentelemetry/sdk/logs/exporter.h"
#include "opentelemetry/sdk/metrics/push_metric_exporter.h"
#include "opentelemetry/sdk/trace/exporter.h"

namespace esp_opentelemetry {

// The exporter-taking esp_opentelemetry_tracing_setup() overload wraps this in
// a BatchSpanProcessor, so a batch - not a single span - is what reaches the
// channel; the writer chunks it to fit the app-trace buffer. Pass a
// SimpleSpanProcessor to the processor-taking overload instead to flush per
// span, which keeps each document small at the cost of an export on every
// span end.
std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> MakeJtagSpanExporter();

std::unique_ptr<opentelemetry::sdk::logs::LogRecordExporter> MakeJtagLogRecordExporter();

std::unique_ptr<ProfilesExporter> MakeJtagProfilesExporter();

std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> MakeJtagMetricExporter();

}  // namespace esp_opentelemetry

#endif  // CONFIG_ESP_OPENTELEMETRY_EXPORTER_JTAG
