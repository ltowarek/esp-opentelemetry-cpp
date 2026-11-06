// OpenTelemetry metrics provider setup for ESP-IDF: a
// PeriodicExportingMetricReader feeding the OTLP/HTTP metric exporter over
// esp_http_client. Applications register their instruments against the global
// meter provider after calling esp_opentelemetry_metrics_setup().

#include "esp_metrics.hpp"

#include "sdkconfig.h"

#ifdef CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED

extern "C" {
#include "esp_log.h"
}

#include "esp_export_thread.hpp"
#if defined(CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_HTTP)
#include "esp_otlp_http_exporters.hpp"
#endif
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h"
#include "opentelemetry/sdk/metrics/meter_context_factory.h"
#include "opentelemetry/sdk/metrics/meter_provider_factory.h"
#include "opentelemetry/sdk/metrics/provider.h"
#include "opentelemetry/sdk/metrics/view/view_registry_factory.h"
#include <chrono>
#include <string>

#endif  // CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED

void observe_double([[maybe_unused]] opentelemetry::metrics::ObserverResult& obs,
                    [[maybe_unused]] double value) {
#ifdef CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED
  opentelemetry::nostd::get<
      opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<double>>>(obs)
      ->Observe(value);
#endif
}

void observe_int64([[maybe_unused]] opentelemetry::metrics::ObserverResult& obs,
                   [[maybe_unused]] int64_t value) {
#ifdef CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED
  opentelemetry::nostd::get<
      opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<int64_t>>>(obs)
      ->Observe(value);
#endif
}

void esp_opentelemetry_metrics_setup(
    [[maybe_unused]] std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> exporter,
    [[maybe_unused]] opentelemetry::sdk::resource::ResourceAttributes resource_attrs) {
#ifdef CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED
  if (exporter == nullptr) {
    ESP_LOGW("esp_otel_metrics", "no metric exporter supplied; metrics disabled.");
    return;
  }

  opentelemetry::sdk::metrics::PeriodicExportingMetricReaderOptions reader_opts;
  reader_opts.export_interval_millis =
      std::chrono::milliseconds(CONFIG_ESP_OPENTELEMETRY_METRICS_EXPORT_INTERVAL_MS);
  reader_opts.export_timeout_millis =
      std::chrono::milliseconds(CONFIG_ESP_OPENTELEMETRY_METRICS_EXPORT_INTERVAL_MS / 2);

  auto resource = opentelemetry::sdk::resource::Resource::Create(resource_attrs);
  auto context = opentelemetry::sdk::metrics::MeterContextFactory::Create(
      opentelemetry::sdk::metrics::ViewRegistryFactory::Create(), resource);
  {
    // The reader spawns its collect/export pthread; run it at low priority.
    esp_opentelemetry::ScopedExportThreadConfig export_thread_cfg(/*prio=*/1);
    auto reader = opentelemetry::sdk::metrics::PeriodicExportingMetricReaderFactory::Create(
        std::move(exporter), reader_opts);
    context->AddMetricReader(std::move(reader));
  }

  auto sdk_provider = opentelemetry::sdk::metrics::MeterProviderFactory::Create(std::move(context));
  std::shared_ptr<opentelemetry::metrics::MeterProvider> api_provider = std::move(sdk_provider);
  opentelemetry::sdk::metrics::Provider::SetMeterProvider(api_provider);
#endif  // CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED
}

void esp_opentelemetry_metrics_setup(
    [[maybe_unused]] opentelemetry::sdk::resource::ResourceAttributes resource_attrs) {
#if defined(CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED) && \
    defined(CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_HTTP)
  const char* metrics_base = CONFIG_ESP_OPENTELEMETRY_METRICS_OTLP_BASE_URL;
  if (*metrics_base == '\0') {
    ESP_LOGW("esp_otel_metrics", "metrics base URL is empty; metrics disabled.");
    return;
  }
  esp_opentelemetry_metrics_setup(
      esp_opentelemetry::MakeOtlpHttpMetricExporter(metrics_base), resource_attrs);
#endif
}
