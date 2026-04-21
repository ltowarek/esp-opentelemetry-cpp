// OpenTelemetry tracing facade for ESP-IDF firmware.
//
// Mirrors controller/src/controller/tracing.py so the W3C traceparent
// produced on the JS/Python side chains cleanly into device-side spans.
//
// Usage:
//
//   esp_opentelemetry_setup(CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME);   // once, after Wi-Fi is up
//   auto tracer = esp_opentelemetry_tracer();
//   auto span = tracer->StartSpan("my.span");
//   auto scope = opentelemetry::trace::Scope(span);
//
// When CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED is off or
// ESP_OPENTELEMETRY_EXPORTER_OTLP_ENDPOINT is empty,
// esp_opentelemetry_setup() leaves the global provider at its default
// (no-op) value; esp_opentelemetry_tracer() still returns a valid tracer
// whose spans are silently dropped.

#pragma once

#include "opentelemetry/context/context.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/trace/tracer.h"

#include <cJSON.h>

// Initialise the global tracer provider, configure the W3C traceparent
// propagator, and wire up an OTLP/HTTP exporter backed by esp_http_client.
// Safe to call multiple times; subsequent calls are ignored.
void esp_opentelemetry_setup(const char* service_name);

// Return the process-wide tracer. Always non-null - falls back to the
// API-level no-op tracer when esp_opentelemetry_setup() has not installed
// a provider.
opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer>
esp_opentelemetry_tracer();

// Inject the active trace context onto a cJSON object as "traceparent" /
// "tracestate" string members, mirroring
// controller.tracing.inject_trace_context.
void esp_opentelemetry_inject_traceparent(cJSON* obj);

// Extract a trace context from a cJSON object's "traceparent" /
// "tracestate" members. Returns the current context when the keys are
// absent. Mirrors controller.tracing.extract_trace_context.
opentelemetry::context::Context
esp_opentelemetry_extract_traceparent(const cJSON* obj);
