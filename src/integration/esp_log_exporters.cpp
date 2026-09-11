#include "esp_log_exporters.hpp"

#if defined(CONFIG_ESP_OPENTELEMETRY_EXPORTER_ESP_LOG)

extern "C" {
#include "esp_log.h"
}

#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/nostd/variant.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED)
#include "opentelemetry/sdk/trace/span_data.h"
#include "opentelemetry/trace/span_metadata.h"
#include "opentelemetry/trace/trace_state.h"
#endif

#if defined(CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED)
#include "opentelemetry/logs/severity.h"
#include "opentelemetry/sdk/logs/read_write_log_record.h"
#endif

#if defined(CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED)
#include "opentelemetry/sdk/metrics/data/metric_data.h"
#include "opentelemetry/sdk/metrics/export/metric_producer.h"
#include "opentelemetry/sdk/resource/resource.h"
#endif

namespace esp_opentelemetry {

namespace {

constexpr const char* kSpanTag = "otel.span";
constexpr const char* kLogTag = "otel.log";
constexpr const char* kMetricTag = "otel.metric";
constexpr const char* kProfileTag = "otel.profile";

// Checked before any formatting: the compile-time cap (this translation
// unit's LOG_LOCAL_LEVEL, i.e. CONFIG_LOG_MAXIMUM_LEVEL) and the runtime tag
// level, the same two gates esp_log itself applies. A tag with no level of
// its own falls back to the default level, exactly as plain ESP_LOGx calls
// do.
bool ShouldEspLog(const char* tag, esp_log_level_t level) {
  if (LOG_LOCAL_LEVEL < static_cast<int>(level)) {
    return false;
  }
  return esp_log_level_get(tag) >= level;
}

// One ESP_LOGx call per line: dispatches to the macro matching a runtime
// level, mirroring esp_logs.cpp's write_console().
void EmitLine(esp_log_level_t level, const char* tag, const std::string& line) {
  switch (level) {
    case ESP_LOG_ERROR:
      ESP_LOGE(tag, "%s", line.c_str());
      break;
    case ESP_LOG_WARN:
      ESP_LOGW(tag, "%s", line.c_str());
      break;
    case ESP_LOG_INFO:
      ESP_LOGI(tag, "%s", line.c_str());
      break;
    case ESP_LOG_DEBUG:
      ESP_LOGD(tag, "%s", line.c_str());
      break;
    case ESP_LOG_VERBOSE:
      ESP_LOGV(tag, "%s", line.c_str());
      break;
    default:
      break;
  }
}

// ---- Value formatting: no C++ streams, everything appended to a heap
// std::string. Mirrors what the Ostream exporters' plain operator<< would
// print for the same variant alternative (no std::boolalpha, so a bare bool
// prints as 1/0 - the SDK's own print_value() never sets that flag either).

void AppendText(std::string& out, opentelemetry::nostd::string_view v) {
  out.append(v.data(), v.size());
}

void AppendDouble(std::string& out, double v) {
  char buf[32];
  const int n = std::snprintf(buf, sizeof(buf), "%g", v);
  if (n > 0) {
    out.append(buf, static_cast<std::size_t>(n));
  }
}

void AppendScalar(std::string& out, bool v) { out += (v ? '1' : '0'); }
void AppendScalar(std::string& out, double v) { AppendDouble(out, v); }
void AppendScalar(std::string& out, const std::string& v) { out += v; }
template <typename T>
void AppendScalar(std::string& out, const T& v) {
  out += std::to_string(v);
}

template <typename T>
void AppendVector(std::string& out, const std::vector<T>& vec) {
  out += '[';
  for (std::size_t i = 0; i < vec.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    AppendScalar(out, static_cast<T>(vec[i]));
  }
  out += ']';
}

struct AppendValueVisitor {
  std::string& out;
  template <typename T>
  void operator()(const T& v) const {
    AppendScalar(out, v);
  }
  template <typename T>
  void operator()(const std::vector<T>& v) const {
    AppendVector(out, v);
  }
};

template <typename Variant>
void AppendAttributeValue(std::string& out, const Variant& value) {
  opentelemetry::nostd::visit(AppendValueVisitor{out}, value);
}

// "{key: value, key2: value2}".
template <typename Map>
void AppendAttributeMap(std::string& out, const Map& map) {
  out += '{';
  bool first = true;
  for (const auto& kv : map) {
    if (!first) {
      out += ", ";
    }
    first = false;
    out += kv.first;
    out += ": ";
    AppendAttributeValue(out, kv.second);
  }
  out += '}';
}

}  // namespace

#if defined(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED)

namespace {

namespace trace_sdk = opentelemetry::sdk::trace;
namespace trace_api = opentelemetry::trace;

const char* SpanKindToString(trace_api::SpanKind kind) {
  switch (kind) {
    case trace_api::SpanKind::kClient:
      return "Client";
    case trace_api::SpanKind::kInternal:
      return "Internal";
    case trace_api::SpanKind::kServer:
      return "Server";
    case trace_api::SpanKind::kProducer:
      return "Producer";
    case trace_api::SpanKind::kConsumer:
      return "Consumer";
  }
  return "";
}

const char* StatusCodeToString(trace_api::StatusCode status) {
  switch (status) {
    case trace_api::StatusCode::kUnset:
      return "Unset";
    case trace_api::StatusCode::kOk:
      return "Ok";
    case trace_api::StatusCode::kError:
      return "Error";
  }
  return "";
}

void AppendResourceAndScope(
    std::string& out, const opentelemetry::sdk::resource::Resource& resource,
    const opentelemetry::sdk::instrumentationscope::InstrumentationScope& scope) {
  out += ", resources: ";
  AppendAttributeMap(out, resource.GetAttributes());
  out += ", instr-lib: ";
  out += scope.GetName();
  if (!scope.GetVersion().empty()) {
    out += "-";
    out += scope.GetVersion();
  }
}

void EmitEventLine(const std::string& span_id, const trace_sdk::SpanDataEvent& event) {
  std::string line = "span_id: " + span_id + ", name: " + event.GetName() +
                      ", timestamp: " +
                      std::to_string(event.GetTimestamp().time_since_epoch().count()) +
                      ", attributes: ";
  AppendAttributeMap(line, event.GetAttributes());
  EmitLine(ESP_LOG_INFO, kSpanTag, line);
}

void EmitLinkLine(const std::string& span_id, const trace_sdk::SpanDataLink& link) {
  char link_trace_id[32] = {0};
  char link_span_id[16] = {0};
  link.GetSpanContext().trace_id().ToLowerBase16(link_trace_id);
  link.GetSpanContext().span_id().ToLowerBase16(link_span_id);

  std::string line = "span_id: " + span_id +
                      ", trace_id: " + std::string(link_trace_id, 32) +
                      ", linked_span_id: " + std::string(link_span_id, 16) +
                      ", tracestate: " + link.GetSpanContext().trace_state()->ToHeader() +
                      ", attributes: ";
  AppendAttributeMap(line, link.GetAttributes());
  EmitLine(ESP_LOG_INFO, kSpanTag, line);
}

class EspLogSpanExporter final : public trace_sdk::SpanExporter {
 public:
  std::unique_ptr<trace_sdk::Recordable> MakeRecordable() noexcept override {
    return std::unique_ptr<trace_sdk::Recordable>(new trace_sdk::SpanData);
  }

  opentelemetry::sdk::common::ExportResult Export(
      const opentelemetry::nostd::span<std::unique_ptr<trace_sdk::Recordable>>& spans) noexcept
      override {
    if (is_shutdown_) {
      return opentelemetry::sdk::common::ExportResult::kFailure;
    }

    for (auto& recordable : spans) {
      auto span = std::unique_ptr<trace_sdk::SpanData>(
          static_cast<trace_sdk::SpanData*>(recordable.release()));
      if (span == nullptr) {
        continue;
      }

      const bool is_error = span->GetStatus() == trace_api::StatusCode::kError;
      const esp_log_level_t level = is_error ? ESP_LOG_WARN : ESP_LOG_INFO;
      if (!ShouldEspLog(kSpanTag, level)) {
        continue;
      }

      char trace_id[32] = {0};
      char span_id[16] = {0};
      char parent_span_id[16] = {0};
      span->GetTraceId().ToLowerBase16(trace_id);
      span->GetSpanId().ToLowerBase16(span_id);
      span->GetParentSpanId().ToLowerBase16(parent_span_id);
      const std::string span_id_str(span_id, 16);

      std::string line = "name: ";
      AppendText(line, span->GetName());
      line += ", trace_id: ";
      line += std::string(trace_id, 32);
      line += ", span_id: ";
      line += span_id_str;
      line += ", tracestate: ";
      line += span->GetSpanContext().trace_state()->ToHeader();
      line += ", parent_span_id: ";
      line += std::string(parent_span_id, 16);
      line += ", start: ";
      line += std::to_string(span->GetStartTime().time_since_epoch().count());
      line += ", duration: ";
      line += std::to_string(span->GetDuration().count());
      line += ", description: ";
      AppendText(line, span->GetDescription());
      line += ", span kind: ";
      line += SpanKindToString(span->GetSpanKind());
      line += ", status: ";
      line += StatusCodeToString(span->GetStatus());
      line += ", attributes: ";
      AppendAttributeMap(line, span->GetAttributes());
      AppendResourceAndScope(line, span->GetResource(), span->GetInstrumentationScope());
      EmitLine(level, kSpanTag, line);

      // Events and links always print at INFO (they are not themselves an
      // "item" with an Error/WARN level of their own), so they need their own
      // gate rather than reusing `level`, which may be WARN here.
      if (ShouldEspLog(kSpanTag, ESP_LOG_INFO)) {
        for (const auto& event : span->GetEvents()) {
          EmitEventLine(span_id_str, event);
        }
        for (const auto& link : span->GetLinks()) {
          EmitLinkLine(span_id_str, link);
        }
      }
    }
    return opentelemetry::sdk::common::ExportResult::kSuccess;
  }

  bool ForceFlush(std::chrono::microseconds /*timeout*/) noexcept override { return true; }

  bool Shutdown(std::chrono::microseconds /*timeout*/) noexcept override {
    is_shutdown_ = true;
    return true;
  }

 private:
  std::atomic<bool> is_shutdown_{false};
};

}  // namespace

std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> MakeEspLogSpanExporter() {
  return std::make_unique<EspLogSpanExporter>();
}

#endif  // CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED

#if defined(CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED)

namespace {

namespace sdk_logs = opentelemetry::sdk::logs;
namespace logs_api = opentelemetry::logs;

// TRACE(1-4)->V, DEBUG(5-8)->D, INFO(9-12)->I, WARN(13-16)->W,
// ERROR(17-20)/FATAL(21-24)->E; kInvalid(0) is not printable.
bool EspLogLevelForSeverity(logs_api::Severity severity, esp_log_level_t* out) {
  const auto n = static_cast<uint8_t>(severity);
  if (n >= 1 && n <= 4) {
    *out = ESP_LOG_VERBOSE;
  } else if (n >= 5 && n <= 8) {
    *out = ESP_LOG_DEBUG;
  } else if (n >= 9 && n <= 12) {
    *out = ESP_LOG_INFO;
  } else if (n >= 13 && n <= 16) {
    *out = ESP_LOG_WARN;
  } else if (n >= 17 && n <= 24) {
    *out = ESP_LOG_ERROR;
  } else {
    return false;
  }
  return true;
}

// A record with a log.tag attribute (the ESP log bridge's original tag) is
// also filtered by that tag's own runtime level; a record with no such
// attribute is gated by kLogTag alone.
bool BridgedTagAllows(const sdk_logs::ReadWriteLogRecord& record, esp_log_level_t level) {
  const auto& attrs = record.GetAttributes();
  const auto it = attrs.find("log.tag");
  if (it == attrs.end()) {
    return true;
  }
  if (!opentelemetry::nostd::holds_alternative<std::string>(it->second)) {
    return true;
  }
  return esp_log_level_get(opentelemetry::nostd::get<std::string>(it->second).c_str()) >= level;
}

class EspLogLogRecordExporter final : public sdk_logs::LogRecordExporter {
 public:
  std::unique_ptr<sdk_logs::Recordable> MakeRecordable() noexcept override {
    return std::unique_ptr<sdk_logs::Recordable>(new sdk_logs::ReadWriteLogRecord());
  }

  opentelemetry::sdk::common::ExportResult Export(
      const opentelemetry::nostd::span<std::unique_ptr<sdk_logs::Recordable>>& records) noexcept
      override {
    if (is_shutdown_) {
      return opentelemetry::sdk::common::ExportResult::kFailure;
    }

    for (auto& recordable : records) {
      auto record = std::unique_ptr<sdk_logs::ReadWriteLogRecord>(
          static_cast<sdk_logs::ReadWriteLogRecord*>(recordable.release()));
      if (record == nullptr) {
        continue;
      }

      esp_log_level_t level;
      if (!EspLogLevelForSeverity(record->GetSeverity(), &level)) {
        continue;
      }
      if (!ShouldEspLog(kLogTag, level) || !BridgedTagAllows(*record, level)) {
        continue;
      }

      char trace_id[32] = {0};
      char span_id[16] = {0};
      char trace_flags[2] = {0};
      record->GetTraceId().ToLowerBase16(trace_id);
      record->GetSpanId().ToLowerBase16(span_id);
      record->GetTraceFlags().ToLowerBase16(trace_flags);

      std::string line = "timestamp: ";
      line += std::to_string(record->GetTimestamp().time_since_epoch().count());
      line += ", observed_timestamp: ";
      line += std::to_string(record->GetObservedTimestamp().time_since_epoch().count());
      line += ", severity_num: ";
      line += std::to_string(static_cast<unsigned>(record->GetSeverity()));
      line += ", severity_text: ";
      // EspLogLevelForSeverity() already rejected anything outside 1-24, so
      // this index is always in range for the 25-entry table.
      AppendText(line, logs_api::SeverityNumToText[static_cast<std::uint32_t>(record->GetSeverity())]);
      line += ", body: ";
      AppendAttributeValue(line, record->GetBody());
      line += ", resource: ";
      AppendAttributeMap(line, record->GetResource().GetAttributes());
      line += ", attributes: ";
      AppendAttributeMap(line, record->GetAttributes());
      line += ", event_id: ";
      line += std::to_string(record->GetEventId());
      line += ", event_name: ";
      AppendText(line, record->GetEventName());
      line += ", trace_id: ";
      line += std::string(trace_id, 32);
      line += ", span_id: ";
      line += std::string(span_id, 16);
      line += ", trace_flags: ";
      line += std::string(trace_flags, 2);
      // The SDK's Ostream log exporter nests four scope sub-fields (name,
      // version, schema_url, attributes); "scope "/"scope " prefixes keep
      // them distinct from the record's own "attributes" field above.
      line += ", scope name: ";
      line += record->GetInstrumentationScope().GetName();
      line += ", version: ";
      line += record->GetInstrumentationScope().GetVersion();
      line += ", schema url: ";
      line += record->GetInstrumentationScope().GetSchemaURL();
      line += ", scope attributes: ";
      AppendAttributeMap(line, record->GetInstrumentationScope().GetAttributes());
      EmitLine(level, kLogTag, line);
    }
    return opentelemetry::sdk::common::ExportResult::kSuccess;
  }

  bool ForceFlush(std::chrono::microseconds /*timeout*/) noexcept override { return true; }

  bool Shutdown(std::chrono::microseconds /*timeout*/) noexcept override {
    is_shutdown_ = true;
    return true;
  }

 private:
  std::atomic<bool> is_shutdown_{false};
};

}  // namespace

std::unique_ptr<opentelemetry::sdk::logs::LogRecordExporter> MakeEspLogLogRecordExporter() {
  return std::make_unique<EspLogLogRecordExporter>();
}

#endif  // CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED

#if defined(CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED)

namespace {

namespace metrics_sdk = opentelemetry::sdk::metrics;

// SumPointData::value_ / LastValuePointData::value_ / HistogramPointData's
// sum_/min_/max_ share this int64_t-or-double variant.
void AppendNumericVariant(std::string& out,
                           const opentelemetry::nostd::variant<int64_t, double>& v) {
  if (opentelemetry::nostd::holds_alternative<double>(v)) {
    AppendDouble(out, opentelemetry::nostd::get<double>(v));
  } else {
    out += std::to_string(opentelemetry::nostd::get<int64_t>(v));
  }
}

std::string TimeToString(opentelemetry::common::SystemTimestamp ts) {
  const std::time_t epoch_time = std::chrono::system_clock::to_time_t(ts);
  struct tm tm_buf {};
  if (gmtime_r(&epoch_time, &tm_buf) == nullptr) {
    return std::string();
  }
  char buf[64];
  const std::size_t n = std::strftime(buf, sizeof(buf), "%c", &tm_buf);
  if (n == 0) {
    return std::string();
  }
  return std::string(buf, n);
}

// Common prefix shared by every data point's line: the instrument fields,
// scope and start/end time. Value + type + attributes + resource follow,
// depending on the point's variant.
void AppendPointPrefix(std::string& out,
                        const opentelemetry::sdk::instrumentationscope::InstrumentationScope& scope,
                        const metrics_sdk::MetricData& metric_data) {
  out += "scope name: ";
  out += scope.GetName();
  out += ", schema url: ";
  out += scope.GetSchemaURL();
  out += ", version: ";
  out += scope.GetVersion();
  out += ", start time: ";
  out += TimeToString(metric_data.start_ts);
  out += ", end time: ";
  out += TimeToString(metric_data.end_ts);
  out += ", instrument name: ";
  out += metric_data.instrument_descriptor.name_;
  out += ", description: ";
  out += metric_data.instrument_descriptor.description_;
  out += ", unit: ";
  out += metric_data.instrument_descriptor.unit_;
}

void AppendPointDataAndAttributes(std::string& out, const metrics_sdk::PointType& point_data,
                                   const metrics_sdk::PointAttributes& attributes) {
  if (opentelemetry::nostd::holds_alternative<metrics_sdk::SumPointData>(point_data)) {
    const auto& p = opentelemetry::nostd::get<metrics_sdk::SumPointData>(point_data);
    out += ", type: SumPointData, value: ";
    AppendNumericVariant(out, p.value_);
  } else if (opentelemetry::nostd::holds_alternative<metrics_sdk::HistogramPointData>(
                 point_data)) {
    const auto& p = opentelemetry::nostd::get<metrics_sdk::HistogramPointData>(point_data);
    out += ", type: HistogramPointData, count: ";
    out += std::to_string(p.count_);
    out += ", sum: ";
    AppendNumericVariant(out, p.sum_);
    if (p.record_min_max_) {
      out += ", min: ";
      AppendNumericVariant(out, p.min_);
      out += ", max: ";
      AppendNumericVariant(out, p.max_);
    }
    out += ", buckets: ";
    AppendVector(out, p.boundaries_);
    out += ", counts: ";
    AppendVector(out, p.counts_);
  } else if (opentelemetry::nostd::holds_alternative<metrics_sdk::LastValuePointData>(
                 point_data)) {
    const auto& p = opentelemetry::nostd::get<metrics_sdk::LastValuePointData>(point_data);
    out += ", type: LastValuePointData, timestamp: ";
    out += std::to_string(p.sample_ts_.time_since_epoch().count());
    // The SDK's ostream exporter sets std::boolalpha before this one field.
    out += ", valid: ";
    out += (p.is_lastvalue_valid_ ? "true" : "false");
    out += ", value: ";
    AppendNumericVariant(out, p.value_);
  } else if (opentelemetry::nostd::holds_alternative<metrics_sdk::Base2ExponentialHistogramPointData>(
                 point_data)) {
    const auto& p =
        opentelemetry::nostd::get<metrics_sdk::Base2ExponentialHistogramPointData>(point_data);
    out += ", type: Base2ExponentialHistogramPointData, count: ";
    out += std::to_string(p.count_);
    out += ", sum: ";
    AppendDouble(out, p.sum_);
    out += ", zero_count: ";
    out += std::to_string(p.zero_count_);
    if (p.record_min_max_) {
      out += ", min: ";
      AppendDouble(out, p.min_);
      out += ", max: ";
      AppendDouble(out, p.max_);
    }
    out += ", scale: ";
    out += std::to_string(p.scale_);
    // positive_buckets_/negative_buckets_ (AdaptingCircularBufferCounter) are
    // omitted: no instrument in this component's examples produces this point
    // type, and the parent spec routes rare metric point types to code review
    // rather than a QEMU check.
  }
  out += ", attributes: ";
  AppendAttributeMap(out, attributes);
}

class EspLogMetricExporter final : public metrics_sdk::PushMetricExporter {
 public:
  metrics_sdk::AggregationTemporality GetAggregationTemporality(
      metrics_sdk::InstrumentType /*instrument_type*/) const noexcept override {
    return metrics_sdk::AggregationTemporality::kCumulative;
  }

  opentelemetry::sdk::common::ExportResult Export(
      const metrics_sdk::ResourceMetrics& data) noexcept override {
    if (is_shutdown_) {
      return opentelemetry::sdk::common::ExportResult::kFailure;
    }

    if (!ShouldEspLog(kMetricTag, ESP_LOG_INFO)) {
      return opentelemetry::sdk::common::ExportResult::kSuccess;
    }

    for (const auto& scope_metrics : data.scope_metric_data_) {
      for (const auto& metric_data : scope_metrics.metric_data_) {
        for (const auto& point_data_attr : metric_data.point_data_attr_) {
          if (opentelemetry::nostd::holds_alternative<metrics_sdk::DropPointData>(
                  point_data_attr.point_data)) {
            continue;
          }
          std::string line;
          AppendPointPrefix(line, *scope_metrics.scope_, metric_data);
          AppendPointDataAndAttributes(line, point_data_attr.point_data,
                                        point_data_attr.attributes);
          line += ", resources: ";
          AppendAttributeMap(line, data.resource_->GetAttributes());
          EmitLine(ESP_LOG_INFO, kMetricTag, line);
        }
      }
    }
    return opentelemetry::sdk::common::ExportResult::kSuccess;
  }

  bool ForceFlush(std::chrono::microseconds /*timeout*/) noexcept override { return true; }

  bool Shutdown(std::chrono::microseconds /*timeout*/) noexcept override {
    is_shutdown_ = true;
    return true;
  }

 private:
  std::atomic<bool> is_shutdown_{false};
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
