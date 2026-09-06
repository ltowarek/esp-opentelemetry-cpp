#include "esp_otlp_http_exporters.hpp"

#if defined(CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_HTTP)

#include "esp_http_client_transport.hpp"
#include "esp_otlp_json_backends.hpp"

extern "C" {
#include "esp_http_client.h"
#include "esp_log.h"
}

#include <chrono>
#include <utility>

#if defined(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED)
#include "opentelemetry/exporters/otlp/otlp_http_exporter_options.h"
#if defined(CONFIG_ESP_OPENTELEMETRY_OTLP_HTTP_ENCODING_PROTOBUF)
#include "opentelemetry/exporters/otlp/otlp_http_exporter.h"
#else
#include "opentelemetry/exporters/otlp/otlp_json_http_exporter.h"
#endif
#endif
#if defined(CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED)
#include "opentelemetry/exporters/otlp/otlp_http_log_record_exporter_options.h"
#if defined(CONFIG_ESP_OPENTELEMETRY_OTLP_HTTP_ENCODING_PROTOBUF)
#include "opentelemetry/exporters/otlp/otlp_http_log_record_exporter.h"
#else
#include "opentelemetry/exporters/otlp/otlp_json_http_log_record_exporter.h"
#endif
#endif
#if defined(CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED)
#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h"
#if defined(CONFIG_ESP_OPENTELEMETRY_OTLP_HTTP_ENCODING_PROTOBUF)
#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter_factory.h"
#else
#include "opentelemetry/exporters/otlp/otlp_json_http_metric_exporter_factory.h"
#endif
#endif

namespace otlp_api = opentelemetry::exporter::otlp;

namespace esp_opentelemetry {

namespace {

constexpr const char* kProfilesTag = "esp_otel_profiles";

// JSON with hex trace/span ids, matching what the collector's OTLP/HTTP
// receiver expects and what the JTAG exporters emit, so the wire form does not
// change with the exporter. The three serialization fields are what the
// protobuf path needs to be told; the OTLP/JSON exporters emit exactly this
// encoding by construction and ignore them, so they are set unconditionally
// rather than per encoding.
template <typename Options>
void ApplyCommon(Options& options, const std::string& url) {
  options.url                = url;
  options.content_type       = otlp_api::HttpRequestContentType::kJson;
  options.json_bytes_mapping = otlp_api::JsonBytesMappingKind::kHexId;
  options.use_json_name      = false;
  options.console_debug      = false;
  options.timeout            = std::chrono::seconds(10);
  options.compression        = "none";
}

}  // namespace

#if defined(CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED)
std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> MakeOtlpHttpSpanExporter(
    const std::string& base_url) {
  otlp_api::OtlpHttpExporterOptions options;
  ApplyCommon(options, base_url + "/v1/traces");
#if defined(CONFIG_ESP_OPENTELEMETRY_OTLP_HTTP_ENCODING_PROTOBUF)
  return std::unique_ptr<opentelemetry::sdk::trace::SpanExporter>(
      new otlp_api::OtlpHttpExporter(options, otlp_api::OtlpHttpExporterRuntimeOptions(),
                                     MakeEspHttpClient(), CjsonJsonWriterFactory()));
#else
  return std::unique_ptr<opentelemetry::sdk::trace::SpanExporter>(
      new otlp_api::OtlpJsonHttpExporter(
          options, otlp_api::OtlpHttpExporterRuntimeOptions(), MakeEspHttpClient(),
          CjsonJsonWriterFactory(), CjsonJsonReaderFactory()));
#endif
}
#endif

#if defined(CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED)
std::unique_ptr<opentelemetry::sdk::logs::LogRecordExporter>
MakeOtlpHttpLogRecordExporter(const std::string& base_url) {
  otlp_api::OtlpHttpLogRecordExporterOptions options;
  ApplyCommon(options, base_url + "/v1/logs");
#if defined(CONFIG_ESP_OPENTELEMETRY_OTLP_HTTP_ENCODING_PROTOBUF)
  return std::unique_ptr<opentelemetry::sdk::logs::LogRecordExporter>(
      new otlp_api::OtlpHttpLogRecordExporter(
          options, otlp_api::OtlpHttpLogRecordExporterRuntimeOptions(),
          MakeEspHttpClient(), CjsonJsonWriterFactory()));
#else
  return std::unique_ptr<opentelemetry::sdk::logs::LogRecordExporter>(
      new otlp_api::OtlpJsonHttpLogRecordExporter(
          options, otlp_api::OtlpHttpLogRecordExporterRuntimeOptions(),
          MakeEspHttpClient(), CjsonJsonWriterFactory(), CjsonJsonReaderFactory()));
#endif
}
#endif

#if defined(CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED)
std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter>
MakeOtlpHttpMetricExporter(const std::string& base_url) {
  otlp_api::OtlpHttpMetricExporterOptions options;
  ApplyCommon(options, base_url + "/v1/metrics");
#if defined(CONFIG_ESP_OPENTELEMETRY_OTLP_HTTP_ENCODING_PROTOBUF)
  return otlp_api::OtlpHttpMetricExporterFactory::Create(
      options, otlp_api::OtlpHttpMetricExporterRuntimeOptions(), MakeEspHttpClient(),
      CjsonJsonWriterFactory());
#else
  return otlp_api::OtlpJsonHttpMetricExporterFactory::Create(
      options, otlp_api::OtlpHttpMetricExporterRuntimeOptions(), MakeEspHttpClient(),
      CjsonJsonWriterFactory(), CjsonJsonReaderFactory());
#endif
}
#endif

namespace {

class OtlpHttpProfilesExporter final : public ProfilesExporter {
 public:
  explicit OtlpHttpProfilesExporter(std::string url) : url_(std::move(url)) {}

  bool Export(const char* body, std::size_t size) noexcept override {
    esp_http_client_config_t cfg = {};
    cfg.url = url_.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 5000;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
      ESP_LOGW(kProfilesTag, "failed to init HTTP client for profiles export");
      return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, static_cast<int>(size));
    const esp_err_t err = esp_http_client_perform(client);
    bool ok = false;
    if (err == ESP_OK) {
      const int status = esp_http_client_get_status_code(client);
      if (status >= 300) {
        ESP_LOGW(kProfilesTag, "profiles export HTTP %d", status);
      } else {
        ok = true;
      }
    } else {
      ESP_LOGW(kProfilesTag, "profiles export failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return ok;
  }

 private:
  const std::string url_;
};

}  // namespace

std::unique_ptr<ProfilesExporter> MakeOtlpHttpProfilesExporter(
    const std::string& base_url) {
  return std::make_unique<OtlpHttpProfilesExporter>(base_url +
                                                    "/v1development/profiles");
}

}  // namespace esp_opentelemetry

#endif  // CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_HTTP
