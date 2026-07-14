// Only compiled when CONFIG_ESP_OPENTELEMETRY_PROFILING_ENABLED (see
// CMakeLists.txt); esp_profiling_stub.cpp provides the disabled-build no-op
// definitions of the public API below.

#include "esp_profiling.hpp"

#include "esp_task_span_slot.hpp"

#include "sdkconfig.h"

extern "C" {
#include "driver/gptimer.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <sys/time.h>
}

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#ifdef CONFIG_ESP_OPENTELEMETRY_PROFILING_DEBUG_LOG
#include <cinttypes>
#include <cstdio>
#endif

namespace {

constexpr const char* TAG = "esp_otel_profiling";

constexpr int kMaxDepth = CONFIG_ESP_OPENTELEMETRY_PROFILING_MAX_DEPTH;
constexpr int kRingCapacity = CONFIG_ESP_OPENTELEMETRY_PROFILING_RING_CAPACITY;
constexpr int kSampleHz = CONFIG_ESP_OPENTELEMETRY_PROFILING_SAMPLE_HZ;
constexpr int kExportIntervalMs = CONFIG_ESP_OPENTELEMETRY_PROFILING_EXPORT_INTERVAL_MS;
constexpr int kMaxUniqueStacks = CONFIG_ESP_OPENTELEMETRY_PROFILING_MAX_UNIQUE_STACKS;
constexpr int kCores = portNUM_PROCESSORS;

constexpr int kTaskNameLen = CONFIG_FREERTOS_MAX_TASK_NAME_LEN;

// One raw stack sample captured by the timer ISR. The task name is copied at
// sample time (the sampled task is the running task, so its TCB is alive) rather
// than storing the handle and resolving later, which would read a dangling
// pointer for tasks deleted before export. The span active on the interrupted
// task (if any) links the sample to its trace (Pyroscope span profiles).
struct Sample {
  uint8_t core;
  uint8_t depth;
  bool has_span;
  uint8_t span_id[8];
  char task[kTaskNameLen];
  uint32_t pc[kMaxDepth];
};

// Lock-free single-producer (ISR) single-consumer (export task) ring, one per
// core. The export task may run on the other core, so the indices carry the
// ordering contract explicitly: the producer's release-store of head publishes
// the sample payload, and its acquire-load of tail pairs with the consumer's
// release-store of tail (which must not be reordered before the consumer's
// payload reads). On Xtensa these compile to plain 32-bit accesses plus a
// memw where ordering is requested — negligible at profiling sample rates,
// and correct by the language standard on any future (e.g. RISC-V multicore)
// target rather than by ESP32-S3-specific store-ordering guarantees. A full
// ring drops the new sample (statistical profiling tolerates loss).
struct Ring {
  Sample* buf;
  std::atomic<uint32_t> head;  // written by ISR
  std::atomic<uint32_t> tail;  // written by export task
};

Ring g_rings[kCores];
gptimer_handle_t g_timers[kCores];
// Indexed by core and written only by that core's ISR; readers sum. A single
// shared counter would be a cross-core read-modify-write race losing counts.
volatile uint32_t g_total_samples[kCores];
volatile uint32_t g_spanned_samples[kCores];

// Aggregated unique stack: identical (core, task, span, pc[]) folded to a
// count. Wraps Sample (rather than duplicating its fields) so there is one
// place — not two structs kept in sync by hand — that defines a stack's
// identity.
struct Aggregate {
  Sample sample;
  uint32_t count;
};

// Touched only by the export task (never the ISRs), so allocated PSRAM-first in
// esp_opentelemetry_profiling_setup() instead of costing ~30 KB of internal RAM.
Aggregate* g_agg;
esp_opentelemetry::ProfileStack* g_export;
int g_agg_count;
// Identity key (core/depth/has_span/span_id/task/pc[0..depth)) -> index into
// g_agg, so aggregate() doesn't linearly rescan every existing entry per
// drained sample. Cleared alongside g_agg_count at the start of each window.
std::unordered_map<std::string, int> g_agg_index;

void* alloc_prefer_psram(size_t size) {
  void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == nullptr) {
    p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
  }
  return p;
}

// Walk the interrupted call stack into `s`. esp_backtrace_get_start() seeds the
// frame at this ISR; the Xtensa window unwind continues through the interrupt
// boundary into the interrupted task (the leaf frames are ISR-dispatch plumbing,
// trimmed downstream). All helpers used here live in IRAM.
void IRAM_ATTR sample_stack(Sample* s, uint8_t core) {
  s->core = core;
  TaskHandle_t task = xTaskGetCurrentTaskHandle();
  const char* name = task ? pcTaskGetName(task) : nullptr;
  if (name != nullptr) {
    strlcpy(s->task, name, sizeof(s->task));
  } else {
    s->task[0] = '?';
    s->task[1] = '\0';
  }

  esp_backtrace_frame_t frame = {};
  esp_backtrace_get_start(&frame.pc, &frame.sp, &frame.next_pc);

  uint8_t d = 0;
  while (d < kMaxDepth) {
    s->pc[d++] = esp_cpu_process_stack_pc(frame.pc);
    if (frame.next_pc == 0 || !esp_backtrace_get_next_frame(&frame)) {
      break;
    }
  }
  s->depth = d;

  // Span active on the interrupted task, if any (lock-free slot read).
  s->has_span = esp_opentelemetry_active_span_id(s->span_id);
}

bool IRAM_ATTR on_timer(gptimer_handle_t, const gptimer_alarm_event_data_t*,
                        void* user_ctx) {
  auto core = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(user_ctx));
  Ring& ring = g_rings[core];
  const uint32_t head = ring.head.load(std::memory_order_relaxed);
  const uint32_t next = (head + 1) % kRingCapacity;
  if (next != ring.tail.load(std::memory_order_acquire)) {  // space available
    sample_stack(&ring.buf[head], core);
    if (ring.buf[head].has_span) {
      g_spanned_samples[core] = g_spanned_samples[core] + 1;
    }
    ring.head.store(next, std::memory_order_release);  // publish the payload
    g_total_samples[core] = g_total_samples[core] + 1;
  }
  return false;
}

// Fold one drained sample into the per-window aggregate table. Identity is
// (core, depth, has_span, span_id, task, pc[0..depth)) — same fields the old
// linear scan compared, just hashed instead of rescanned. `task` goes into
// the key via its null-terminated length (std::string(const char*)), not the
// full kTaskNameLen buffer, since the trailing bytes past the terminator are
// uninitialized (strlcpy doesn't zero-fill) and must not affect the key,
// mirroring the strncmp-on-C-string comparison this replaces.
void aggregate(const Sample& s) {
  std::string key;
  key.reserve(sizeof(s.core) + sizeof(s.depth) + sizeof(s.has_span) +
              (s.has_span ? sizeof(s.span_id) : 0) + kTaskNameLen +
              s.depth * sizeof(uint32_t));
  key.append(reinterpret_cast<const char*>(&s.core), sizeof(s.core));
  key.append(reinterpret_cast<const char*>(&s.depth), sizeof(s.depth));
  key.append(reinterpret_cast<const char*>(&s.has_span), sizeof(s.has_span));
  if (s.has_span) {
    key.append(reinterpret_cast<const char*>(s.span_id), sizeof(s.span_id));
  }
  key.append(s.task);
  key.append(reinterpret_cast<const char*>(s.pc), s.depth * sizeof(uint32_t));

  auto it = g_agg_index.find(key);
  if (it != g_agg_index.end()) {
    ++g_agg[it->second].count;
    return;
  }
  if (g_agg_count >= kMaxUniqueStacks) {
    return;  // table full; drop (rare)
  }
  int idx = g_agg_count++;
  g_agg[idx].sample = s;
  g_agg[idx].count = 1;
  g_agg_index.emplace(std::move(key), idx);
}

// Export the aggregated window as OpenTelemetry profiles (raw addresses; the
// host symbolizer resolves them). CONFIG_ESP_OPENTELEMETRY_PROFILING_DEBUG_LOG
// additionally logs each stack so the sampler can be validated on hardware
// with addr2line.
void publish(int64_t window_start_ns, int64_t window_dur_ns) {
  for (int i = 0; i < g_agg_count; ++i) {
    const Aggregate& a = g_agg[i];
    const Sample& s = a.sample;
    g_export[i].core = s.core;
    g_export[i].depth = s.depth;
    g_export[i].count = a.count;
    g_export[i].task_name = s.task;
    g_export[i].addresses = s.pc;
    g_export[i].has_span = s.has_span;
    std::memcpy(g_export[i].span_id, s.span_id, sizeof(g_export[i].span_id));

#ifdef CONFIG_ESP_OPENTELEMETRY_PROFILING_DEBUG_LOG
    char line[16 + kMaxDepth * 11];
    int n = 0;
    for (int d = 0; d < s.depth; ++d) {
      n += snprintf(line + n, sizeof(line) - n, " 0x%08" PRIx32, s.pc[d]);
    }
    ESP_LOGI(TAG, "core=%u task=%s count=%" PRIu32 " stack:%s", s.core, s.task,
             a.count, line);
#endif
  }

  esp_opentelemetry::export_profiles(g_export, static_cast<std::size_t>(g_agg_count),
                                     window_start_ns, window_dur_ns);
}

// Fold all buffered samples from every core's ring into the aggregate table.
// Called frequently so the rings never overflow between exports.
void drain_rings() {
  for (int c = 0; c < kCores; ++c) {
    Ring& ring = g_rings[c];
    uint32_t tail = ring.tail.load(std::memory_order_relaxed);
    const uint32_t head = ring.head.load(std::memory_order_acquire);
    while (tail != head) {
      aggregate(ring.buf[tail]);
      tail = (tail + 1) % kRingCapacity;
      ring.tail.store(tail, std::memory_order_release);
    }
  }
}

// Drain often enough that the ring (kRingCapacity) cannot fill between drains
// even at the maximum sample rate, while publishing only once per export
// window. 50 ms keeps < kRingCapacity samples buffered for rates up to ~1 kHz.
constexpr int kDrainIntervalMs = 50;

// Wall-clock epoch nanoseconds (valid once SNTP has synced; the backend
// rejects profiles whose timestamp is outside its ingestion window).
int64_t wall_clock_ns() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return static_cast<int64_t>(tv.tv_sec) * 1'000'000'000LL +
         static_cast<int64_t>(tv.tv_usec) * 1'000LL;
}

[[noreturn]] void export_task(void*) {
  int64_t mono_start = esp_timer_get_time();
  // Wall-clock time at the start of the current window, not at publish time —
  // time_unix_nano must mark when the samples were taken, not when the window
  // closed kExportIntervalMs later.
  int64_t wall_start = wall_clock_ns();
  int64_t next_publish = mono_start + kExportIntervalMs * 1000LL;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(kDrainIntervalMs));
    drain_rings();

    int64_t mono_now = esp_timer_get_time();
    if (mono_now >= next_publish) {
      if (g_agg_count > 0) {
        publish(wall_start, (mono_now - mono_start) * 1000LL);
      }
      g_agg_count = 0;
      g_agg_index.clear();
      mono_start = mono_now;
      wall_start = wall_clock_ns();
      next_publish = mono_now + kExportIntervalMs * 1000LL;
    }
  }
}

// gptimer's interrupt is installed on the core that calls gptimer_enable(), so
// each timer is armed from a task pinned to its core. Runs once then exits.
void start_timer_on_core(void* arg) {
  auto core = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(arg));

  gptimer_config_t cfg = {};
  cfg.clk_src = GPTIMER_CLK_SRC_DEFAULT;
  cfg.direction = GPTIMER_COUNT_UP;
  cfg.resolution_hz = 1'000'000;  // 1 MHz -> 1 us ticks
  ESP_ERROR_CHECK(gptimer_new_timer(&cfg, &g_timers[core]));

  gptimer_event_callbacks_t cbs = {};
  cbs.on_alarm = on_timer;
  ESP_ERROR_CHECK(gptimer_register_event_callbacks(
      g_timers[core], &cbs, reinterpret_cast<void*>(core)));

  gptimer_alarm_config_t alarm = {};
  alarm.alarm_count = 1'000'000 / kSampleHz;
  alarm.reload_count = 0;
  alarm.flags.auto_reload_on_alarm = true;
  ESP_ERROR_CHECK(gptimer_set_alarm_action(g_timers[core], &alarm));

  ESP_ERROR_CHECK(gptimer_enable(g_timers[core]));
  ESP_ERROR_CHECK(gptimer_start(g_timers[core]));
  vTaskDelete(nullptr);
}

}  // namespace

void esp_opentelemetry_profiling_setup() {
  static bool started = false;
  if (started) {
    return;
  }
  started = true;

  // Mirror activated spans into per-task slots so samples can be linked to
  // the span active on the interrupted task (span->profile linking). Owned
  // here, not by esp_opentelemetry_tracing_setup(), since profiling is the only
  // consumer of the span slot on both ends (this call installs it; the
  // sampling ISR below is its only reader) — this works whether or not
  // tracing itself is enabled.
  esp_opentelemetry::install_task_span_context_storage();

  g_agg = static_cast<Aggregate*>(alloc_prefer_psram(sizeof(Aggregate) * kMaxUniqueStacks));
  g_export = static_cast<esp_opentelemetry::ProfileStack*>(
      alloc_prefer_psram(sizeof(esp_opentelemetry::ProfileStack) * kMaxUniqueStacks));
  // Not assert(): builds with CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE
  // compile it away, which would leave g_agg/g_export null and let the ISR
  // below dereference them on the first sample.
  if (g_agg == nullptr || g_export == nullptr) {
    ESP_LOGE(TAG, "profiling: failed to allocate aggregation tables");
    abort();
  }

  for (int c = 0; c < kCores; ++c) {
    // The rings are written by the ISRs and stay in internal RAM.
    g_rings[c].buf = static_cast<Sample*>(
        heap_caps_malloc(sizeof(Sample) * kRingCapacity, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (g_rings[c].buf == nullptr) {
      ESP_LOGE(TAG, "profiling: failed to allocate ring buffer for core %d", c);
      abort();
    }
    g_rings[c].head.store(0, std::memory_order_relaxed);
    g_rings[c].tail.store(0, std::memory_order_relaxed);
  }

  for (int c = 0; c < kCores; ++c) {
    xTaskCreatePinnedToCore(start_timer_on_core, "prof_arm", 3072,
                            reinterpret_cast<void*>(c), 5, nullptr, c);
  }

  // The export task builds OTLP/JSON (cJSON + std containers) and runs the HTTP
  // client, so it needs a generous stack.
  xTaskCreate(export_task, "prof_export", 8192, nullptr, 1, nullptr);

  ESP_LOGI(TAG, "profiling started: %d Hz/core, depth %d, export every %d ms",
           kSampleHz, kMaxDepth, kExportIntervalMs);
}

uint32_t esp_opentelemetry_profiling_samples() {
  uint32_t sum = 0;
  for (int c = 0; c < kCores; ++c) {
    sum += g_total_samples[c];
  }
  return sum;
}

uint32_t esp_opentelemetry_profiling_spanned_samples() {
  uint32_t sum = 0;
  for (int c = 0; c < kCores; ++c) {
    sum += g_spanned_samples[c];
  }
  return sum;
}
