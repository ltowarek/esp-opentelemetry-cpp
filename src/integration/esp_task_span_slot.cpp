#include "esp_task_span_slot.hpp"

#include "sdkconfig.h"

extern "C" {
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

#include "opentelemetry/context/runtime_context.h"
#include "opentelemetry/trace/context.h"
#include "opentelemetry/trace/span_context.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

// This file only serves the profiling sampler and is only compiled when
// CONFIG_ESP_OPENTELEMETRY_PROFILING_ENABLED (see CMakeLists.txt). The slot
// lives in a FreeRTOS thread-local-storage pointer. Index 0 belongs to
// ESP-IDF's pthread TLS, so we need index 1, which only exists when the app
// sets CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS >= 2. Deletion callbacks
// (default y) free the record when a task dies.
// Fail the build, not silently: without the slot, span->profile linking never
// works and there is no way to notice short of comparing flame graphs, so a
// missing prerequisite must not compile to a working-looking binary.
#if !((configNUM_THREAD_LOCAL_STORAGE_POINTERS >= 2) && \
      defined(CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS))
#error "CONFIG_ESP_OPENTELEMETRY_PROFILING_ENABLED requires CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS >= 2 and CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS (see examples/profiling/*/sdkconfig.defaults)"
#endif

namespace {

constexpr BaseType_t kSpanSlotTlsIndex = 1;  // 0 = pthread TLS

// Written only by the owning task; read only by the timer ISR that interrupts
// that task on the same core -> no cross-core access. The seqlock guards
// against the single remaining hazard: the ISR firing mid-write.
// last_stamped_span_id is owner-task-only bookkeeping (never read by the ISR)
// used to avoid re-stamping pyroscope.profile.id on re-activation.
struct SpanSlot {
  volatile uint32_t seq;  // odd while the owner is writing
  bool valid;
  uint8_t span_id[8];
  uint8_t last_stamped_span_id[8];
};

void delete_slot(int /*index*/, void* slot) { std::free(slot); }

// Task context only (lazily allocates; never called from ISR).
SpanSlot* slot_for_current_task() {
  TaskHandle_t task = xTaskGetCurrentTaskHandle();
  auto* slot = static_cast<SpanSlot*>(
      pvTaskGetThreadLocalStoragePointer(task, kSpanSlotTlsIndex));
  if (slot == nullptr) {
    slot = static_cast<SpanSlot*>(std::calloc(1, sizeof(SpanSlot)));
    if (slot == nullptr) {
      return nullptr;
    }
    vTaskSetThreadLocalStoragePointerAndDelCallback(task, kSpanSlotTlsIndex,
                                                    slot, delete_slot);
  }
  return slot;
}

// Mirror the current context's span into the task slot. Seqlock write; the
// fences are compiler barriers (same-core ISR needs no hardware barrier).
void update_slot_from(const opentelemetry::context::Context& ctx) {
  SpanSlot* slot = slot_for_current_task();
  if (slot == nullptr) {
    return;
  }
  const opentelemetry::trace::SpanContext sc =
      opentelemetry::trace::GetSpan(ctx)->GetContext();

  const uint32_t seq = slot->seq + 1;
  slot->seq = seq;  // odd: write in progress
  std::atomic_signal_fence(std::memory_order_release);
  if (sc.IsValid()) {
    std::memcpy(slot->span_id, sc.span_id().Id().data(), sizeof(slot->span_id));
    slot->valid = true;
  } else {
    slot->valid = false;
  }
  std::atomic_signal_fence(std::memory_order_release);
  slot->seq = seq + 1;  // even: consistent
}

// Stamp the span with the CONFIG_ESP_OPENTELEMETRY_PROFILES_SPAN_ATTRIBUTE
// attribute (span id as hex) so a trace backend can link the span to its
// profile samples — by default the Grafana span-profiles convention
// (pyroscope.profile.id); empty key disables. Done on activation so exactly
// the spans that can own CPU samples get it. No-op for API-level
// DefaultSpan/no-op spans. The OTLP recordable APPENDS on every SetAttribute,
// so re-activations of the span most recently stamped on this task are
// skipped to avoid duplicate attributes (a span re-activated after a
// different span may still be stamped twice — rare and harmless).
void stamp_profile_id(const opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>& span) {
  constexpr const char* kAttributeKey = CONFIG_ESP_OPENTELEMETRY_PROFILES_SPAN_ATTRIBUTE;
  if (kAttributeKey[0] == '\0') {
    return;
  }
  const opentelemetry::trace::SpanContext sc = span->GetContext();
  if (!sc.IsValid()) {
    return;
  }
  SpanSlot* slot = slot_for_current_task();
  if (slot != nullptr &&
      std::memcmp(slot->last_stamped_span_id, sc.span_id().Id().data(),
                  sizeof(slot->last_stamped_span_id)) == 0) {
    return;
  }
  char hex[2 * opentelemetry::trace::SpanId::kSize + 1] = {};
  sc.span_id().ToLowerBase16(
      opentelemetry::nostd::span<char, 2 * opentelemetry::trace::SpanId::kSize>(
          hex, 2 * opentelemetry::trace::SpanId::kSize));
  span->SetAttribute(kAttributeKey, hex);
  if (slot != nullptr) {
    std::memcpy(slot->last_stamped_span_id, sc.span_id().Id().data(),
                sizeof(slot->last_stamped_span_id));
  }
}

class EspTaskSpanContextStorage final
    : public opentelemetry::context::ThreadLocalContextStorage {
 public:
  opentelemetry::nostd::unique_ptr<opentelemetry::context::Token> Attach(
      const opentelemetry::context::Context& context) noexcept override {
    auto token = ThreadLocalContextStorage::Attach(context);
    stamp_profile_id(opentelemetry::trace::GetSpan(context));
    update_slot_from(GetCurrent());
    return token;
  }

  bool Detach(opentelemetry::context::Token& token) noexcept override {
    const bool ok = ThreadLocalContextStorage::Detach(token);
    update_slot_from(GetCurrent());
    return ok;
  }
};

}  // namespace

namespace esp_opentelemetry {

void install_task_span_context_storage() {
  opentelemetry::context::RuntimeContext::SetRuntimeContextStorage(
      opentelemetry::nostd::shared_ptr<opentelemetry::context::RuntimeContextStorage>(
          new EspTaskSpanContextStorage()));
}

}  // namespace esp_opentelemetry

bool IRAM_ATTR esp_opentelemetry_active_span_id(uint8_t span_id[8]) {
  TaskHandle_t task = xTaskGetCurrentTaskHandle();
  if (task == nullptr) {
    return false;
  }
  const auto* slot = static_cast<const SpanSlot*>(
      pvTaskGetThreadLocalStoragePointer(task, kSpanSlotTlsIndex));
  if (slot == nullptr) {
    return false;
  }
  const uint32_t seq = slot->seq;
  if ((seq & 1U) != 0U) {
    return false;  // owner mid-write; skip (bounded, no spin)
  }
  std::atomic_signal_fence(std::memory_order_acquire);
  const bool valid = slot->valid;
  uint8_t sid[8];
  std::memcpy(sid, slot->span_id, sizeof(sid));
  std::atomic_signal_fence(std::memory_order_acquire);
  if (slot->seq != seq || !valid) {
    return false;  // torn or no active span
  }
  if (span_id != nullptr) {
    std::memcpy(span_id, sid, sizeof(sid));
  }
  return true;
}
