#include "esp_log_exporters.hpp"

#if defined(CONFIG_ESP_OPENTELEMETRY_EXPORTER_ESP_LOG)

extern "C" {
#include "esp_log.h"
}

#include <chrono>
#include <cstddef>
#include <memory>
#include <ostream>
#include <streambuf>
#include <string>

#if defined(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED)
#include "opentelemetry/exporters/ostream/span_exporter.h"
#endif

#if defined(CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED)
#include "opentelemetry/exporters/ostream/log_record_exporter.h"
#endif

#if defined(CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED)
#include "opentelemetry/exporters/ostream/metric_exporter.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#endif

namespace esp_opentelemetry {

namespace {

constexpr const char* kSpanTag = "otel.span";
constexpr const char* kLogTag = "otel.log";
constexpr const char* kMetricTag = "otel.metric";
constexpr const char* kProfileTag = "otel.profile";

// Checked before formatting a profile document (the one signal this file
// still formats itself): the compile-time cap (this translation unit's
// LOG_LOCAL_LEVEL, i.e. CONFIG_LOG_MAXIMUM_LEVEL) and the runtime tag level,
// the same two gates esp_log itself applies.
bool ShouldEspLog(const char* tag, esp_log_level_t level) {
  if (LOG_LOCAL_LEVEL < static_cast<int>(level)) {
    return false;
  }
  return esp_log_level_get(tag) >= level;
}

// Stands in for std::cout for an Ostream exporter: every character the
// exporter writes is buffered here and, on '\n', flushed as one ESP_LOGI call
// under `tag`. The Ostream exporters have no per-item level to carry through
// (no WARN-on-Error, no log-severity mapping), so every line prints at INFO -
// esp_log's own tag/level gate still applies to each line, just after the
// line has already been built rather than before.
class EspLogStreambuf final : public std::streambuf {
 public:
  explicit EspLogStreambuf(const char* tag) : tag_(tag) {}

 protected:
  int_type overflow(int_type ch) override {
    if (!traits_type::eq_int_type(ch, traits_type::eof())) {
      const char c = traits_type::to_char_type(ch);
      if (c == '\n') {
        ESP_LOGI(tag_, "%s", line_.c_str());
        line_.clear();
      } else {
        line_.push_back(c);
      }
    }
    return ch;
  }

 private:
  const char* tag_;
  std::string line_;
};

}  // namespace

#if defined(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED)

namespace {

namespace trace_sdk = opentelemetry::sdk::trace;

class EspLogSpanExporter final : public trace_sdk::SpanExporter {
 public:
  EspLogSpanExporter() : streambuf_(kSpanTag), stream_(&streambuf_), inner_(stream_) {}

  std::unique_ptr<trace_sdk::Recordable> MakeRecordable() noexcept override {
    return inner_.MakeRecordable();
  }

  opentelemetry::sdk::common::ExportResult Export(
      const opentelemetry::nostd::span<std::unique_ptr<trace_sdk::Recordable>>& spans) noexcept
      override {
    return inner_.Export(spans);
  }

  bool ForceFlush(std::chrono::microseconds timeout) noexcept override {
    return inner_.ForceFlush(timeout);
  }

  bool Shutdown(std::chrono::microseconds timeout) noexcept override {
    return inner_.Shutdown(timeout);
  }

 private:
  EspLogStreambuf streambuf_;
  std::ostream stream_;
  opentelemetry::exporter::trace::OStreamSpanExporter inner_;
};

}  // namespace

std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> MakeEspLogSpanExporter() {
  return std::make_unique<EspLogSpanExporter>();
}

#endif  // CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED

#if defined(CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED)

namespace {

namespace sdk_logs = opentelemetry::sdk::logs;

class EspLogLogRecordExporter final : public sdk_logs::LogRecordExporter {
 public:
  EspLogLogRecordExporter() : streambuf_(kLogTag), stream_(&streambuf_), inner_(stream_) {}

  std::unique_ptr<sdk_logs::Recordable> MakeRecordable() noexcept override {
    return inner_.MakeRecordable();
  }

  opentelemetry::sdk::common::ExportResult Export(
      const opentelemetry::nostd::span<std::unique_ptr<sdk_logs::Recordable>>& records) noexcept
      override {
    return inner_.Export(records);
  }

  bool ForceFlush(std::chrono::microseconds timeout) noexcept override {
    return inner_.ForceFlush(timeout);
  }

  bool Shutdown(std::chrono::microseconds timeout) noexcept override {
    return inner_.Shutdown(timeout);
  }

 private:
  EspLogStreambuf streambuf_;
  std::ostream stream_;
  opentelemetry::exporter::logs::OStreamLogRecordExporter inner_;
};

}  // namespace

std::unique_ptr<opentelemetry::sdk::logs::LogRecordExporter> MakeEspLogLogRecordExporter() {
  return std::make_unique<EspLogLogRecordExporter>();
}

#endif  // CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED

#if defined(CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED)

namespace {

namespace metrics_sdk = opentelemetry::sdk::metrics;

class EspLogMetricExporter final : public metrics_sdk::PushMetricExporter {
 public:
  EspLogMetricExporter()
      : streambuf_(kMetricTag),
        stream_(&streambuf_),
        inner_(stream_, metrics_sdk::AggregationTemporality::kCumulative) {}

  metrics_sdk::AggregationTemporality GetAggregationTemporality(
      metrics_sdk::InstrumentType instrument_type) const noexcept override {
    return inner_.GetAggregationTemporality(instrument_type);
  }

  opentelemetry::sdk::common::ExportResult Export(
      const metrics_sdk::ResourceMetrics& data) noexcept override {
    return inner_.Export(data);
  }

  bool ForceFlush(std::chrono::microseconds timeout) noexcept override {
    return inner_.ForceFlush(timeout);
  }

  bool Shutdown(std::chrono::microseconds timeout) noexcept override {
    return inner_.Shutdown(timeout);
  }

 private:
  EspLogStreambuf streambuf_;
  std::ostream stream_;
  opentelemetry::exporter::metrics::OStreamMetricExporter inner_;
};

}  // namespace

std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> MakeEspLogMetricExporter() {
  return std::make_unique<EspLogMetricExporter>();
}

#endif  // CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED

namespace {

class EspLogProfilesExporter final : public ProfilesExporter {
 public:
  bool Export(const char* body, std::size_t size) noexcept override {
    if (!ShouldEspLog(kProfileTag, ESP_LOG_INFO)) {
      return true;
    }
    ESP_LOGI(kProfileTag, "%.*s", static_cast<int>(size), body);
    return true;
  }
};

}  // namespace

std::unique_ptr<ProfilesExporter> MakeEspLogProfilesExporter() {
  return std::make_unique<EspLogProfilesExporter>();
}

}  // namespace esp_opentelemetry

#endif  // CONFIG_ESP_OPENTELEMETRY_EXPORTER_ESP_LOG
