#include "esp_jtag_exporters.hpp"

#if defined(CONFIG_ESP_OPENTELEMETRY_EXPORTER_JTAG)

#include "esp_jtag_channel.hpp"

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
#include <memory>

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

class JtagProfilesExporter final : public ProfilesExporter {
 public:
  bool Export(const char* body, std::size_t size) noexcept override {
    return jtag_channel::WriteDocument(body, size);
  }
};

}  // namespace

#if defined(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED)

std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> MakeJtagSpanExporter() {
  return std::unique_ptr<opentelemetry::sdk::trace::SpanExporter>(
      new otlp_api::OtlpFileExporter(AppTraceOptions<otlp_api::OtlpFileExporterOptions>()));
}

#endif  // CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED

#if defined(CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED)

std::unique_ptr<opentelemetry::sdk::logs::LogRecordExporter> MakeJtagLogRecordExporter() {
  return std::unique_ptr<opentelemetry::sdk::logs::LogRecordExporter>(
      new otlp_api::OtlpFileLogRecordExporter(
          AppTraceOptions<otlp_api::OtlpFileLogRecordExporterOptions>()));
}

#endif  // CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED

std::unique_ptr<ProfilesExporter> MakeJtagProfilesExporter() {
  return std::make_unique<JtagProfilesExporter>();
}

#if defined(CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED)

std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> MakeJtagMetricExporter() {
  return std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter>(
      new otlp_api::OtlpFileMetricExporter(
          AppTraceOptions<otlp_api::OtlpFileMetricExporterOptions>()));
}

#endif  // CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED

}  // namespace esp_opentelemetry

#endif  // CONFIG_ESP_OPENTELEMETRY_EXPORTER_JTAG
