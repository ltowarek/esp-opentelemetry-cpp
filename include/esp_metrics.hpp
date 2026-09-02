// OpenTelemetry metrics facade for ESP-IDF firmware.
//
// Usage:
//
//   esp_opentelemetry_metrics_setup({{"service.name", CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME}});   // once, after Wi-Fi is up
//   auto meter = opentelemetry::metrics::Provider::GetMeterProvider()
//                    ->GetMeter(CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME);
//   auto counter = meter->CreateUInt64Counter("my.counter");
//
// The exporter is the caller's choice, as it is upstream. The no-exporter
// overload builds an OTLP/HTTP one from
// CONFIG_ESP_OPENTELEMETRY_METRICS_OTLP_BASE_URL.
//
// When CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED is off, or no exporter is
// supplied and that URL is empty, esp_opentelemetry_metrics_setup() leaves the
// global meter provider at its default (no-op) value; instruments still
// register but their recordings are silently dropped.

#pragma once

#include "opentelemetry/sdk/metrics/push_metric_exporter.h"

#include <memory>

#include <cstdint>

#include "opentelemetry/metrics/observer_result.h"
#include "opentelemetry/sdk/resource/resource.h"

// Install the global meter provider: a PeriodicExportingMetricReader
// (CONFIG_ESP_OPENTELEMETRY_METRICS_EXPORT_INTERVAL_MS) feeding the OTLP/HTTP
// metric exporter at CONFIG_ESP_OPENTELEMETRY_METRICS_OTLP_BASE_URL. A no-op
// when CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED is off or the base URL is
// empty — instruments registered via the API's no-op provider are silently
// dropped. resource_attrs becomes the meter's resource as-is.
// Initialise the global meter provider with the given exporter, wrapped in a
// PeriodicExportingMetricReader. The exporter is the caller's choice, as it is
// upstream - see esp_otlp_http_exporters.hpp and esp_jtag_exporters.hpp.
void esp_opentelemetry_metrics_setup(
    std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> exporter,
    opentelemetry::sdk::resource::ResourceAttributes resource_attrs = {});

// Convenience overload: exports over OTLP/HTTP to
// CONFIG_ESP_OPENTELEMETRY_METRICS_OTLP_BASE_URL. Does nothing when empty.
void esp_opentelemetry_metrics_setup(
    opentelemetry::sdk::resource::ResourceAttributes resource_attrs = {});

// Convenience over the ObserverResult variant API for asynchronous-gauge
// callbacks. Safe (no-op) when metrics are disabled.
void observe_double(opentelemetry::metrics::ObserverResult& obs, double value);
void observe_int64(opentelemetry::metrics::ObserverResult& obs, int64_t value);
