#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_opentelemetry.hpp"
#include "opentelemetry/trace/default_span.h"
#include "opentelemetry/trace/scope.h"
#include "opentelemetry/trace/span_context.h"

extern "C" {
#include "esp_random.h"
}

namespace trace_api = opentelemetry::trace;

// This variant has no Wi-Fi and no tracing backend, so there is no real
// tracer to draw a span from — enabling one here would pull in the SDK's
// network-touching exporter without ever bringing up Wi-Fi, and is what
// examples/profiling/otlp is for. A SpanContext with a random id stands in
// for one instead (esp_fill_random(), the same primitive the SDK's own id
// generator would use): any span - real or otherwise - that is active via
// trace::Scope while a sample is taken gets linked the same way, through the
// per-task active-span slot.
static opentelemetry::nostd::shared_ptr<trace_api::Span> make_span()
{
    uint8_t trace_id[16];
    uint8_t span_id[8];
    esp_fill_random(trace_id, sizeof(trace_id));
    esp_fill_random(span_id, sizeof(span_id));
    trace_api::SpanContext sc(
        trace_api::TraceId(opentelemetry::nostd::span<const uint8_t, 16>(trace_id)),
        trace_api::SpanId(opentelemetry::nostd::span<const uint8_t, 8>(span_id)),
        trace_api::TraceFlags(trace_api::TraceFlags::kIsSampled), false);
    return opentelemetry::nostd::shared_ptr<trace_api::Span>(new trace_api::DefaultSpan(sc));
}

// A recognizable hot function: with the sampler running, this dominates the
// dumped profiles. noinline so the symbolized frame carries this exact name.
static uint64_t g_sink;
static void __attribute__((noinline)) burn_cpu()
{
    uint64_t acc = 0;
    for (int i = 0; i < 200000; i = i + 1) {
        acc += static_cast<uint64_t>(i) * i;
    }
    g_sink = g_sink + acc;
}

extern "C" void app_main()
{
    // Profiling is fully self-contained: this one call starts the sampler
    // and installs the span-linking slot. No tracing setup needed.
    esp_opentelemetry_profiling_setup();

    // Burn CPU under a span forever: samples land in the sampler's rings,
    // aggregate per (task, span, stack), and export as OTLP/JSON to the
    // serial console (CONFIG_ESP_OPENTELEMETRY_PROFILES_DEBUG_JSON), between
    // PROFILE_JSON_BEGIN/END markers - no Wi-Fi or backend required.
    trace_api::Scope scope(make_span());
    for (;;) {
        burn_cpu();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
