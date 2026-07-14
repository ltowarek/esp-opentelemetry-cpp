// Statistical CPU profiler facade for ESP-IDF firmware.
//
// Usage:
//
//   esp_opentelemetry_profiling_setup();   // starts the sampler; self-contained,
//                                          // no tracing setup required
//
// A per-core timer ISR samples the interrupted call stack (esp_backtrace) at
// CONFIG_ESP_OPENTELEMETRY_PROFILING_SAMPLE_HZ; a low-priority task aggregates
// identical stacks into counts (pprof semantics) over an export window and
// exports them as OpenTelemetry profiles (see export_profiles below). Samples
// are labelled with the FreeRTOS task and, when a span is active
// (trace::Scope) on the sampled task, its span id — so flame graphs split per
// task and link to traces (Pyroscope span profiles) with no other signal
// needing to be enabled.
//
// Opt-in via CONFIG_ESP_OPENTELEMETRY_PROFILING_ENABLED; a no-op when disabled.
// Intended for on-target debugging of where CPU time goes — not for always-on
// production.

#pragma once

#include <cstddef>
#include <cstdint>

// Starts the sampler and installs the per-task span-linking slot. Idempotent;
// safe to call multiple times.
void esp_opentelemetry_profiling_setup();

// Total stack samples captured since startup (0 when profiling is disabled),
// and the subset whose interrupted task had an activated span. Monotonic;
// intended for tests and sanity checks.
uint32_t esp_opentelemetry_profiling_samples();
uint32_t esp_opentelemetry_profiling_spanned_samples();

namespace esp_opentelemetry {

// One aggregated, leaf-first call stack with a sample count, for the profiles
// exporter. Addresses are raw firmware program counters; the host symbolizer
// resolves them against the build ELF.
struct ProfileStack {
  uint8_t core;
  uint8_t depth;
  uint32_t count;
  const char* task_name;      // FreeRTOS task name; may be nullptr
  const uint32_t* addresses;  // length == depth
  // Span active while these samples were taken (Pyroscope span profiles).
  bool has_span;
  uint8_t span_id[8];
};

// Build an OpenTelemetry profiles (v1development) ProfilesData document from the
// aggregated stacks and POST it as OTLP/HTTP JSON to
// CONFIG_ESP_OPENTELEMETRY_PROFILES_OTLP_BASE_URL. Mirrors the trace/metric OTLP/HTTP
// exporters (same JSON-over-esp_http_client transport). One Mapping carries the
// firmware build_id (app_elf_sha256); addresses are left unsymbolized for the
// host symbolizer. No-op when the endpoint is unset.
void export_profiles(const ProfileStack* stacks, std::size_t count,
                     int64_t time_unix_nano, int64_t duration_nano);

}  // namespace esp_opentelemetry
