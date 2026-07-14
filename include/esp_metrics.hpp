// OpenTelemetry metrics facade for ESP-IDF firmware.
//
// Usage:
//
//   esp_opentelemetry_metrics_setup();   // once, after Wi-Fi is up
//   auto meter = opentelemetry::metrics::Provider::GetMeterProvider()
//                    ->GetMeter(CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME);
//   auto counter = meter->CreateUInt64Counter("my.counter");
//
// When CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED is off or
// CONFIG_ESP_OPENTELEMETRY_METRICS_OTLP_BASE_URL is empty,
// esp_opentelemetry_metrics_setup() leaves the global meter provider at its
// default (no-op) value; instruments still register but their
// recordings are silently dropped.

#pragma once

#include <cstdint>

#include "opentelemetry/metrics/observer_result.h"

// Install the global meter provider: a PeriodicExportingMetricReader
// (CONFIG_ESP_OPENTELEMETRY_METRICS_EXPORT_INTERVAL_MS) feeding the OTLP/HTTP
// metric exporter at CONFIG_ESP_OPENTELEMETRY_METRICS_OTLP_BASE_URL. A no-op
// when CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED is off or the base URL is
// empty — instruments registered via the API's no-op provider are silently
// dropped.
void esp_opentelemetry_metrics_setup();

// Convenience over the ObserverResult variant API for asynchronous-gauge
// callbacks. Safe (no-op) when metrics are disabled.
void observe_double(opentelemetry::metrics::ObserverResult& obs, double value);
void observe_int64(opentelemetry::metrics::ObserverResult& obs, int64_t value);
