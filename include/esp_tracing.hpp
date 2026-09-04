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
#include "opentelemetry/sdk/trace/exporter.h"
#include "opentelemetry/sdk/trace/processor.h"
#include "opentelemetry/trace/tracer.h"

#include <memory>

// Initialise the global tracer provider with the given exporter, wrap it in a
// BatchSpanProcessor whose export thread gets a PSRAM stack, and configure the
// W3C traceparent propagator. resource_attrs becomes the tracer's resource
// as-is. Safe to call multiple times; subsequent calls are ignored.
//
// The exporter is the caller's choice, as it is upstream — see
// esp_otlp_http_exporters.hpp and esp_jtag_exporters.hpp for the ESP-specific
// ones, or use any exporter from the SDK.
void esp_opentelemetry_tracing_setup(
    std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> exporter,
    opentelemetry::sdk::resource::ResourceAttributes resource_attrs = {});

// Initialise the global tracer provider with the given processor and
// configure the W3C traceparent propagator. resource_attrs becomes the
// tracer's resource as-is. Safe to call multiple times; subsequent calls are
// ignored.
//
// Use this overload to install a SimpleSpanProcessor, or any other processor
// the SDK provides, instead of the BatchSpanProcessor the exporter-taking
// overload always installs. Unlike that overload, this one does not give the
// processor a PSRAM export thread stack - only relevant for a processor that
// spawns one, such as a hand-built BatchSpanProcessor.
void esp_opentelemetry_tracing_setup(
    std::unique_ptr<opentelemetry::sdk::trace::SpanProcessor> processor,
    opentelemetry::sdk::resource::ResourceAttributes resource_attrs = {});

// Convenience overload: exports over OTLP/HTTP to
// CONFIG_ESP_OPENTELEMETRY_TRACING_OTLP_BASE_URL. Does nothing when that URL
// is empty. Equivalent to passing MakeOtlpHttpSpanExporter(that URL).
void esp_opentelemetry_tracing_setup(
    opentelemetry::sdk::resource::ResourceAttributes resource_attrs = {});

// Return the process-wide tracer. Always non-null - falls back to the
// API-level no-op tracer when esp_opentelemetry_tracing_setup() has not installed
// a provider.
opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer>
esp_opentelemetry_tracer();
