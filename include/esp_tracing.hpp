// OpenTelemetry tracing facade for ESP-IDF firmware.
//
// Usage:
//
//   esp_opentelemetry_tracing_setup({{"service.name", CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME}});   // once, after Wi-Fi is up
//   auto tracer = esp_opentelemetry_tracer();
//   auto span = tracer->StartSpan("my.span");
//   auto scope = opentelemetry::trace::Scope(span);
//
// When CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED is off or
// ESP_OPENTELEMETRY_TRACING_OTLP_BASE_URL is empty,
// esp_opentelemetry_tracing_setup() leaves the global provider at its default
// (no-op) value; esp_opentelemetry_tracer() still returns a valid tracer
// whose spans are silently dropped.

#pragma once

#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/trace/tracer.h"

// Initialise the global tracer provider, configure the W3C traceparent
// propagator, and wire up an OTLP/HTTP exporter backed by esp_http_client.
// resource_attrs becomes the tracer's resource as-is. Safe to call
// multiple times; subsequent calls are ignored.
void esp_opentelemetry_tracing_setup(
    opentelemetry::sdk::resource::ResourceAttributes resource_attrs = {});

// Return the process-wide tracer. Always non-null - falls back to the
// API-level no-op tracer when esp_opentelemetry_tracing_setup() has not installed
// a provider.
opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer>
esp_opentelemetry_tracer();
