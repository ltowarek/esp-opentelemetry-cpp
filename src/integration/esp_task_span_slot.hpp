// Per-task active-span slot: the FreeRTOS analog of Go's goroutine pprof labels.
//
// A RuntimeContext storage subclass mirrors the currently *activated* span
// (opentelemetry::trace::Scope) of each task into a small per-task record that
// the profiling timer ISR can read lock-free. See esp_task_span_slot.cpp for
// the ISR-safety argument.
//
// Everything here is opentelemetry API-header-only (no SDK), so it compiles and
// works even when CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED is off (tests use
// trace::DefaultSpan).

#pragma once

#include <cstdint>

namespace esp_opentelemetry {

// Replace the process RuntimeContext storage with the task-slot-mirroring
// implementation. Must run before the first span activation; called from
// esp_opentelemetry_profiling_setup(), the sole consumer of the span slot
// (installer and reader both live in profiling). A missing FreeRTOS TLS
// pointer slot is a build-time #error in the .cpp, not a runtime fallback.
void install_task_span_context_storage();

}  // namespace esp_opentelemetry

// Read the id of the span currently activated (trace::Scope) on the *calling*
// task, as mirrored into its task slot by install_task_span_context_storage().
// ISR-safe and lock-free — intended to be called from the profiling timer ISR
// for the task it interrupted. Returns false when no span is active (or
// mid-update; the caller records the sample as unlinked). Internal to this
// component (profiling is the only caller); not part of the public API.
bool esp_opentelemetry_active_span_id(uint8_t span_id[8]);
