// OpenTelemetry logs provider setup for ESP-IDF: a BatchLogRecordProcessor
// feeding the OTLP/HTTP log record exporter over esp_http_client, plus the
// bridge esp_log_otel.h's ESP_LOGx wrappers funnel into.

// write_console() re-emits a line whose level the call site already checked
// against its own LOG_LOCAL_LEVEL, so this translation unit must not apply a
// second, lower cap of its own. Must precede any esp_log.h include.
#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE

#include "esp_logs.hpp"
#include "esp_log_otel_emit.h"

#include "sdkconfig.h"

#include "opentelemetry/logs/provider.h"

#if defined(CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED)
#include "esp_export_thread.hpp"
#if defined(CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_HTTP)
#include "esp_otlp_http_exporters.hpp"
#endif

extern "C" {
#include "esp_log.h"
}

#include "opentelemetry/common/key_value_iterable_view.h"
#include "opentelemetry/logs/severity.h"
#include "opentelemetry/sdk/logs/batch_log_record_processor.h"
#include "opentelemetry/sdk/logs/batch_log_record_processor_options.h"
#include "opentelemetry/sdk/logs/logger_provider_factory.h"
#include "opentelemetry/sdk/logs/processor.h"
#include "opentelemetry/sdk/logs/provider.h"

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#endif  // CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED

#include <atomic>

namespace logs_api = opentelemetry::logs;

namespace {

std::atomic<bool> g_initialised{false};

constexpr const char* kLoggerName    = "esp-opentelemetry-cpp";
constexpr const char* kLoggerVersion = "1.0.0";

}  // namespace

#if defined(CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED)

namespace sdk_logs = opentelemetry::sdk::logs;
namespace sdk_res  = opentelemetry::sdk::resource;

static constexpr const char* TAG = "esp_otel_logs";

namespace {

// esp_log_level_t is ordered NONE, ERROR, WARN, INFO, DEBUG, VERBOSE.
logs_api::Severity SeverityFromEspLogLevel(esp_log_level_t level) {
  switch (level) {
    case ESP_LOG_ERROR:
      return logs_api::Severity::kError;
    case ESP_LOG_WARN:
      return logs_api::Severity::kWarn;
    case ESP_LOG_INFO:
      return logs_api::Severity::kInfo;
    case ESP_LOG_DEBUG:
      return logs_api::Severity::kDebug;
    case ESP_LOG_VERBOSE:
      return logs_api::Severity::kTrace;
    default:
      return logs_api::Severity::kInvalid;
  }
}

// Same-task re-entry: emitting a record can itself log (a full queue, an
// allocation failure), and that log would emit another record. The export
// thread's own logging is kept out of the loop separately, by the component's
// sources not taking the ESP_LOGx redefinition.
thread_local bool t_emitting = false;

struct ReentryGuard {
  ReentryGuard() { t_emitting = true; }
  ~ReentryGuard() { t_emitting = false; }
};

}  // namespace

#endif  // CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED

void esp_opentelemetry_logs_setup(
    std::unique_ptr<opentelemetry::sdk::logs::LogRecordExporter> exporter,
    opentelemetry::sdk::resource::ResourceAttributes resource_attrs) {
#if defined(CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED)
  if (exporter == nullptr) {
    ESP_LOGW(TAG, "no log record exporter supplied; logs disabled.");
    return;
  }

  sdk_logs::BatchLogRecordProcessorOptions batch_options;
  batch_options.max_queue_size = CONFIG_ESP_OPENTELEMETRY_LOGS_BATCH_MAX_QUEUE_SIZE;
  batch_options.schedule_delay_millis =
      std::chrono::milliseconds(CONFIG_ESP_OPENTELEMETRY_LOGS_BATCH_SCHEDULE_DELAY_MS);

  std::unique_ptr<sdk_logs::LogRecordProcessor> processor;
  {
    // The BatchLogRecordProcessor constructor spawns its export pthread.
    esp_opentelemetry::ScopedExportThreadConfig export_thread_cfg;
    processor = std::unique_ptr<sdk_logs::LogRecordProcessor>(
        new sdk_logs::BatchLogRecordProcessor(std::move(exporter), batch_options));
  }

  esp_opentelemetry_logs_setup(std::move(processor), std::move(resource_attrs));
#else
  (void)exporter;
  (void)resource_attrs;
#endif  // CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED
}

void esp_opentelemetry_logs_setup(
    std::unique_ptr<opentelemetry::sdk::logs::LogRecordProcessor> processor,
    opentelemetry::sdk::resource::ResourceAttributes resource_attrs) {
#if defined(CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED)
  if (processor == nullptr) {
    ESP_LOGW(TAG, "no log record processor supplied; logs disabled.");
    return;
  }

  bool expected = false;
  if (!g_initialised.compare_exchange_strong(expected, true)) {
    return;
  }

  auto resource = sdk_res::Resource::Create(resource_attrs);
  auto provider = sdk_logs::LoggerProviderFactory::Create(std::move(processor), resource);
  std::shared_ptr<logs_api::LoggerProvider> api_provider = std::move(provider);
  sdk_logs::Provider::SetLoggerProvider(api_provider);

  ESP_LOGI(TAG, "OpenTelemetry logs enabled");
#else
  (void)processor;
  (void)resource_attrs;
#endif  // CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED
}

void esp_opentelemetry_logs_setup(
    opentelemetry::sdk::resource::ResourceAttributes resource_attrs) {
#if defined(CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED) && \
    defined(CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_HTTP)
  const std::string endpoint = CONFIG_ESP_OPENTELEMETRY_LOGS_OTLP_BASE_URL;
  if (endpoint.empty()) {
    ESP_LOGW(TAG, "logs base URL is empty; logs disabled.");
    return;
  }
  esp_opentelemetry_logs_setup(
      esp_opentelemetry::MakeOtlpHttpLogRecordExporter(endpoint), resource_attrs);
#else
  (void)resource_attrs;
#endif
}


opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger>
esp_opentelemetry_logger() {
  auto provider = logs_api::Provider::GetLoggerProvider();
  return provider->GetLogger(kLoggerName, "", kLoggerVersion);
}

#if defined(CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED)

namespace {

// The console half of the bridge. esp_logs.cpp deliberately does not include
// esp_log_otel.h, so these are ESP-IDF's own macros: decoration, timestamp and
// runtime tag filtering are unchanged. LOG_LOCAL_LEVEL is raised for this
// translation unit (see the top of the file) because the call site's own cap
// has already been applied by the time we get here.
void write_console(esp_log_level_t level, const char* tag, const char* body) {
  switch (level) {
    case ESP_LOG_ERROR:
      ESP_LOGE(tag, "%s", body);
      break;
    case ESP_LOG_WARN:
      ESP_LOGW(tag, "%s", body);
      break;
    case ESP_LOG_INFO:
      ESP_LOGI(tag, "%s", body);
      break;
    case ESP_LOG_DEBUG:
      ESP_LOGD(tag, "%s", body);
      break;
    case ESP_LOG_VERBOSE:
      ESP_LOGV(tag, "%s", body);
      break;
    default:
      break;
  }
}

}  // namespace
#endif  // CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED

extern "C" void esp_opentelemetry_log_and_emit(esp_log_level_t level,
                                               const char* tag,
                                               const char* file, int line,
                                               const char* function,
                                               const char* format, ...) {
#if defined(CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED)
  char body[CONFIG_ESP_OPENTELEMETRY_LOGS_MAX_BODY_LEN];
  va_list args;
  va_start(args, format);
  const int written = vsnprintf(body, sizeof(body), format, args);
  va_end(args);
  if (written < 0) {
    return;
  }

  write_console(level, tag, body);

  if (t_emitting) {
    return;
  }
  const logs_api::Severity severity = SeverityFromEspLogLevel(level);
  if (severity == logs_api::Severity::kInvalid) {
    return;
  }

  // Resolved per call rather than cached: whichever provider is installed at
  // this moment wins, so a consumer that swaps the global provider (as the
  // ostream example does, and as tests do) is honoured without a rebind step.
  // The SDK's GetLogger() is a name lookup over the provider's existing
  // loggers, not a construction.
  ReentryGuard guard;
  esp_opentelemetry_logger()->EmitLogRecord(
      severity, opentelemetry::nostd::string_view(body),
      opentelemetry::common::MakeAttributes(
          {{"log.tag", tag},
           {"code.filepath", file},
           {"code.lineno", static_cast<int32_t>(line)},
           {"code.function", function}}));
#else
  (void)level;
  (void)tag;
  (void)file;
  (void)line;
  (void)function;
  (void)format;
#endif  // CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED
}
