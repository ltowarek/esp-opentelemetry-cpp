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
//       std::make_unique<esp_opentelemetry::JtagSpanExporter>(), resource);
//
// One class per signal. All four share the single app-trace channel, one whole
// document at a time, so concurrent signals cannot interleave into an
// unparseable line. The trace/logs/metrics classes are compiled only when their
// signal is enabled; JtagProfilesExporter is not, being a transport for a JSON
// document rather than a hook into the profiler.
//
// Pass one to the matching esp_opentelemetry_*_setup() call, or to a provider
// you build yourself - the application chooses its exporter, as it does
// upstream.
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

#include "opentelemetry/nostd/span.h"
#include "opentelemetry/sdk/common/exporter_utils.h"

#include <chrono>
#include <memory>

#if defined(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED)
#include "opentelemetry/sdk/trace/exporter.h"
#include "opentelemetry/sdk/trace/recordable.h"
#endif

#if defined(CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED)
#include "opentelemetry/sdk/logs/exporter.h"
#include "opentelemetry/sdk/logs/recordable.h"
#endif

#if defined(CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED)
#include "opentelemetry/sdk/metrics/push_metric_exporter.h"
#endif

namespace esp_opentelemetry {

#if defined(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED)
// The exporter-taking esp_opentelemetry_tracing_setup() overload wraps this in
// a BatchSpanProcessor, so a batch - not a single span - is what reaches the
// channel; the writer chunks it to fit the app-trace buffer. Pass a
// SimpleSpanProcessor to the processor-taking overload instead to flush per
// span, which keeps each document small at the cost of an export on every
// span end.
class JtagSpanExporter final : public opentelemetry::sdk::trace::SpanExporter {
 public:
  JtagSpanExporter();
  ~JtagSpanExporter() override;

  std::unique_ptr<opentelemetry::sdk::trace::Recordable> MakeRecordable() noexcept override;

  opentelemetry::sdk::common::ExportResult Export(
      const opentelemetry::nostd::span<
          std::unique_ptr<opentelemetry::sdk::trace::Recordable>>& spans) noexcept override;

  bool ForceFlush(std::chrono::microseconds timeout =
                      (std::chrono::microseconds::max)()) noexcept override;

  bool Shutdown(std::chrono::microseconds timeout =
                    (std::chrono::microseconds::max)()) noexcept override;

 private:
  // OtlpFileExporter with an app-trace backend. Held by base-class pointer so
  // this header does not drag the OTLP exporter and protobuf headers into
  // every translation unit that constructs the exporter.
  std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> impl_;
};
#endif  // CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED

#if defined(CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED)
class JtagLogRecordExporter final : public opentelemetry::sdk::logs::LogRecordExporter {
 public:
  JtagLogRecordExporter();
  ~JtagLogRecordExporter() override;

  std::unique_ptr<opentelemetry::sdk::logs::Recordable> MakeRecordable() noexcept override;

  opentelemetry::sdk::common::ExportResult Export(
      const opentelemetry::nostd::span<
          std::unique_ptr<opentelemetry::sdk::logs::Recordable>>& records) noexcept override;

  bool ForceFlush(std::chrono::microseconds timeout =
                      (std::chrono::microseconds::max)()) noexcept override;

  bool Shutdown(std::chrono::microseconds timeout =
                    (std::chrono::microseconds::max)()) noexcept override;

 private:
  std::unique_ptr<opentelemetry::sdk::logs::LogRecordExporter> impl_;
};
#endif  // CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED

class JtagProfilesExporter final : public ProfilesExporter {
 public:
  bool Export(const char* body, std::size_t size) noexcept override;
};

#if defined(CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED)
class JtagMetricExporter final : public opentelemetry::sdk::metrics::PushMetricExporter {
 public:
  JtagMetricExporter();
  ~JtagMetricExporter() override;

  opentelemetry::sdk::metrics::AggregationTemporality GetAggregationTemporality(
      opentelemetry::sdk::metrics::InstrumentType instrument_type) const noexcept override;

  opentelemetry::sdk::common::ExportResult Export(
      const opentelemetry::sdk::metrics::ResourceMetrics& data) noexcept override;

  bool ForceFlush(std::chrono::microseconds timeout =
                      (std::chrono::microseconds::max)()) noexcept override;

  bool Shutdown(std::chrono::microseconds timeout =
                    (std::chrono::microseconds::max)()) noexcept override;

 private:
  std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> impl_;
};
#endif  // CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED

}  // namespace esp_opentelemetry

#endif  // CONFIG_ESP_OPENTELEMETRY_EXPORTER_JTAG
