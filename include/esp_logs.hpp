// OpenTelemetry logs facade for ESP-IDF firmware.
//
// Usage:
//
//   esp_opentelemetry_logs_setup({{"service.name", CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME}});   // once, after Wi-Fi is up
//   auto logger = esp_opentelemetry_logger();
//   logger->Info("something happened");
//
// To ship a translation unit's existing ESP_LOGI/W/E call sites through the
// logger without touching their text, include esp_log_otel.h after esp_log.h.
//
// Set the system time (SNTP) before emitting records. Log records carry
// absolute timestamps and the ESP32 has no clock of its own, so until the time
// is set every record is stamped with seconds since boot, which reads as 1970;
// Loki and other backends reject timestamps that old and drop the record.
//
// When CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED is off or
// CONFIG_ESP_OPENTELEMETRY_LOGS_OTLP_BASE_URL is empty,
// esp_opentelemetry_logs_setup() leaves the global logger provider at its
// default (no-op) value; esp_opentelemetry_logger() still returns a valid
// logger whose records are silently dropped.

#pragma once

#include "opentelemetry/logs/logger.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/sdk/resource/resource.h"

// Install the global logger provider: a BatchLogRecordProcessor
// (CONFIG_ESP_OPENTELEMETRY_LOGS_BATCH_*) feeding the OTLP/HTTP log record
// exporter at CONFIG_ESP_OPENTELEMETRY_LOGS_OTLP_BASE_URL. A no-op when
// CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED is off or the base URL is empty.
// resource_attrs becomes the logger's resource as-is. Safe to call multiple
// times; subsequent calls are ignored.
void esp_opentelemetry_logs_setup(
    opentelemetry::sdk::resource::ResourceAttributes resource_attrs = {});

// Return the process-wide logger. Always non-null - falls back to the
// API-level no-op logger when esp_opentelemetry_logs_setup() has not
// installed a provider.
opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger>
esp_opentelemetry_logger();
