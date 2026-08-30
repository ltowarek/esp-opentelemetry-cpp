// OpenTelemetry logs standing alone: an OStreamLogRecordExporter prints every
// record to the serial console, so the example needs no network. The ESP_LOGx
// call sites are ordinary ESP-IDF logging — including esp_log_otel.h is the
// only change that ships them.

#include "esp_log.h"
#include "esp_log_otel.h"

#include "esp_opentelemetry.hpp"
#include "opentelemetry/exporters/ostream/log_record_exporter_factory.h"
#include "opentelemetry/sdk/logs/logger_provider_factory.h"
#include "opentelemetry/sdk/logs/processor.h"
#include "opentelemetry/sdk/logs/provider.h"
#include "opentelemetry/sdk/logs/simple_log_record_processor_factory.h"
#include "opentelemetry/sdk/resource/resource.h"

#include "capped.hpp"

#include <memory>

namespace {
constexpr const char* TAG = "logs_example";
}

extern "C" void app_main() {
  auto exporter = opentelemetry::exporter::logs::OStreamLogRecordExporterFactory::Create();
  auto processor = opentelemetry::sdk::logs::SimpleLogRecordProcessorFactory::Create(
      std::move(exporter));
  auto resource = opentelemetry::sdk::resource::Resource::Create(
      {{"service.name", CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME}});
  auto provider = opentelemetry::sdk::logs::LoggerProviderFactory::Create(
      std::move(processor), resource);
  std::shared_ptr<opentelemetry::logs::LoggerProvider> api_provider = std::move(provider);
  opentelemetry::sdk::logs::Provider::SetLoggerProvider(api_provider);

  ESP_LOGI(TAG, "info from %s", "app_main");
  ESP_LOGW(TAG, "warn from app_main");
  ESP_LOGE(TAG, "error from app_main");
  // Not wrapped: D/V stay console-only, and this build compiles them out
  // entirely (CONFIG_LOG_MAXIMUM_LEVEL_INFO).
  ESP_LOGD(TAG, "debug from app_main");

  log_from_capped_module();
}
