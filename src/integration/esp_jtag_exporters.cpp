#include "esp_jtag_exporters.hpp"

#if defined(CONFIG_ESP_OPENTELEMETRY_EXPORTER_JTAG)

#include "esp_jtag_channel.hpp"
#include "esp_otlp_json_backends.hpp"

#include "opentelemetry/exporters/otlp/otlp_file_client_options.h"
#include "opentelemetry/nostd/shared_ptr.h"

#if defined(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED)
#include "opentelemetry/exporters/otlp/otlp_file_exporter.h"
#include "opentelemetry/exporters/otlp/otlp_file_exporter_options.h"
#endif

#if defined(CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED)
#include "opentelemetry/exporters/otlp/otlp_file_log_record_exporter.h"
#include "opentelemetry/exporters/otlp/otlp_file_log_record_exporter_options.h"
#endif

#if defined(CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED)
#include "opentelemetry/exporters/otlp/otlp_file_metric_exporter.h"
#include "opentelemetry/exporters/otlp/otlp_file_metric_exporter_options.h"
#endif

#include <cstddef>

namespace otlp_api = opentelemetry::exporter::otlp;

namespace esp_opentelemetry {

namespace {

// OtlpFileAppender that hands each complete document to the shared app-trace
// writer. OtlpFileClient calls Export() once per request document, already
// newline-terminated, so the host side is a plain NDJSON stream.
class AppTraceAppender final : public otlp_api::OtlpFileAppender {
 public:
  void Export(opentelemetry::nostd::string_view data,
              std::size_t /*record_count*/) noexcept override {
    (void)jtag_channel::WriteDocument(data.data(), data.size());
  }

  bool ForceFlush(std::chrono::microseconds timeout) noexcept override {
    return jtag_channel::Flush(timeout);
  }

  bool Shutdown(std::chrono::microseconds timeout) noexcept override {
    return jtag_channel::Flush(timeout);
  }
};

// One appender for every signal: the exporters are independent objects but
// there is a single app-trace channel behind them, and the writer serialises
// on a whole document.
opentelemetry::nostd::shared_ptr<otlp_api::OtlpFileAppender> SharedAppender() {
  static opentelemetry::nostd::shared_ptr<otlp_api::OtlpFileAppender> appender(
      new AppTraceAppender());
  return appender;
}

template <typename Options>
Options AppTraceOptions() {
  Options options;
  options.backend_options = SharedAppender();
  options.console_debug = false;
  return options;
}

}  // namespace

#if defined(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED)

JtagSpanExporter::JtagSpanExporter()
    : impl_(new otlp_api::OtlpFileExporter(
          AppTraceOptions<otlp_api::OtlpFileExporterOptions>(),
          CjsonJsonWriterFactory())) {}

JtagSpanExporter::~JtagSpanExporter() = default;

std::unique_ptr<opentelemetry::sdk::trace::Recordable>
JtagSpanExporter::MakeRecordable() noexcept {
  return impl_->MakeRecordable();
}

opentelemetry::sdk::common::ExportResult JtagSpanExporter::Export(
    const opentelemetry::nostd::span<
        std::unique_ptr<opentelemetry::sdk::trace::Recordable>>& spans) noexcept {
  return impl_->Export(spans);
}

bool JtagSpanExporter::ForceFlush(std::chrono::microseconds timeout) noexcept {
  return impl_->ForceFlush(timeout);
}

bool JtagSpanExporter::Shutdown(std::chrono::microseconds timeout) noexcept {
  return impl_->Shutdown(timeout);
}

#endif  // CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED

#if defined(CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED)

JtagLogRecordExporter::JtagLogRecordExporter()
    : impl_(new otlp_api::OtlpFileLogRecordExporter(
          AppTraceOptions<otlp_api::OtlpFileLogRecordExporterOptions>(),
          CjsonJsonWriterFactory())) {}

JtagLogRecordExporter::~JtagLogRecordExporter() = default;

std::unique_ptr<opentelemetry::sdk::logs::Recordable>
JtagLogRecordExporter::MakeRecordable() noexcept {
  return impl_->MakeRecordable();
}

opentelemetry::sdk::common::ExportResult JtagLogRecordExporter::Export(
    const opentelemetry::nostd::span<
        std::unique_ptr<opentelemetry::sdk::logs::Recordable>>& records) noexcept {
  return impl_->Export(records);
}

bool JtagLogRecordExporter::ForceFlush(std::chrono::microseconds timeout) noexcept {
  return impl_->ForceFlush(timeout);
}

bool JtagLogRecordExporter::Shutdown(std::chrono::microseconds timeout) noexcept {
  return impl_->Shutdown(timeout);
}

#endif  // CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED

bool JtagProfilesExporter::Export(const char* body, std::size_t size) noexcept {
  return jtag_channel::WriteDocument(body, size);
}

#if defined(CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED)

JtagMetricExporter::JtagMetricExporter()
    : impl_(new otlp_api::OtlpFileMetricExporter(
          AppTraceOptions<otlp_api::OtlpFileMetricExporterOptions>(),
          CjsonJsonWriterFactory())) {}

JtagMetricExporter::~JtagMetricExporter() = default;

opentelemetry::sdk::metrics::AggregationTemporality
JtagMetricExporter::GetAggregationTemporality(
    opentelemetry::sdk::metrics::InstrumentType instrument_type) const noexcept {
  return impl_->GetAggregationTemporality(instrument_type);
}

opentelemetry::sdk::common::ExportResult JtagMetricExporter::Export(
    const opentelemetry::sdk::metrics::ResourceMetrics& data) noexcept {
  return impl_->Export(data);
}

bool JtagMetricExporter::ForceFlush(std::chrono::microseconds timeout) noexcept {
  return impl_->ForceFlush(timeout);
}

bool JtagMetricExporter::Shutdown(std::chrono::microseconds timeout) noexcept {
  return impl_->Shutdown(timeout);
}

#endif  // CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED

}  // namespace esp_opentelemetry

#endif  // CONFIG_ESP_OPENTELEMETRY_EXPORTER_JTAG
