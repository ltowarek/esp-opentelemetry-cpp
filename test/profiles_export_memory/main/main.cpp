// A profile export must leave internal RAM to the tasks running next to it.
//
// A std::mutex constructed while internal RAM is exhausted gets no FreeRTOS
// semaphore: its constructor calls pthread_mutex_init() and ignores the error,
// so the mutex keeps whatever its storage held and asserts on its first lock or
// its destruction. The document exported here (MAX_UNIQUE_STACKS stacks of
// MAX_DEPTH distinct frames, every sample span-linked) is the largest the
// profiler can produce.

#include "esp_profiling.hpp"

#include <cJSON.h>

extern "C" {
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
}

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>

namespace {

constexpr int kStacks = CONFIG_ESP_OPENTELEMETRY_PROFILING_MAX_UNIQUE_STACKS;
constexpr int kDepth = CONFIG_ESP_OPENTELEMETRY_PROFILING_MAX_DEPTH;
constexpr const char* kTaskNames[] = {"main",    "IDLE0", "IDLE1", "tiT", "httpd", "sys_evt",
                                      "wifi",    "prof_export"};
constexpr int kTaskCount = sizeof(kTaskNames) / sizeof(kTaskNames[0]);
constexpr int64_t kTimeUnixNano = 1'700'000'000'000'000'000LL;
constexpr int64_t kDurationNano = 5'000'000'000LL;

// The body is copied into PSRAM so capturing it takes nothing from the RAM the
// export must leave alone.
char* g_body = nullptr;

class CapturingExporter final : public esp_opentelemetry::ProfilesExporter {
 public:
  bool Export(const char* body, std::size_t size) noexcept override {
    heap_caps_free(g_body);
    g_body = static_cast<char*>(heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM));
    if (g_body == nullptr) {
      return false;
    }
    std::memcpy(g_body, body, size);
    g_body[size] = '\0';
    return true;
  }
};

esp_opentelemetry::ProfileStack* g_stacks = nullptr;

void make_worst_case_stacks() {
  g_stacks = static_cast<esp_opentelemetry::ProfileStack*>(
      heap_caps_calloc(kStacks, sizeof(esp_opentelemetry::ProfileStack), MALLOC_CAP_SPIRAM));
  auto* addresses = static_cast<uint32_t*>(
      heap_caps_malloc(sizeof(uint32_t) * kStacks * kDepth, MALLOC_CAP_SPIRAM));
  TEST_ASSERT_NOT_NULL(g_stacks);
  TEST_ASSERT_NOT_NULL(addresses);
  for (int i = 0; i < kStacks; ++i) {
    for (int d = 0; d < kDepth; ++d) {
      addresses[i * kDepth + d] = 0x42000000U + 4U * static_cast<uint32_t>(i * kDepth + d);
    }
    esp_opentelemetry::ProfileStack& s = g_stacks[i];
    s.core = static_cast<uint8_t>(i % 2);
    s.depth = kDepth;
    s.count = static_cast<uint32_t>(i + 1);
    s.task_name = kTaskNames[i % kTaskCount];
    s.addresses = &addresses[i * kDepth];
    s.has_span = true;
    for (int b = 0; b < 8; ++b) {
      s.span_id[b] = static_cast<uint8_t>((i >> (8 * (b % 2))) + b);
    }
  }
}

void export_worst_case() {
  esp_opentelemetry::export_profiles(g_stacks, kStacks, kTimeUnixNano, kDurationNano);
}

struct MutexConstructor {
  TaskHandle_t owner;
  std::atomic<bool> stop{false};
  std::atomic<uint32_t> constructed{0};
  std::atomic<uint32_t> without_semaphore{0};
};

// Runs on the core the export does not use, so mutexes are constructed all
// through it, as other tasks would.
void construct_mutexes(void* arg) {
  auto* constructor = static_cast<MutexConstructor*>(arg);
  while (!constructor->stop.load()) {
    // Zeroed storage, so a mutex whose pthread_mutex_init() failed shows a
    // null handle rather than leftover bytes.
    alignas(std::mutex) unsigned char storage[sizeof(std::mutex)] = {};
    auto* mutex = new (storage) std::mutex();
    if (*mutex->native_handle() == 0) {
      constructor->without_semaphore.fetch_add(1);
    }
    mutex->~mutex();
    constructor->constructed.fetch_add(1);
    vTaskDelay(1);
  }
  xTaskNotifyGive(constructor->owner);
  vTaskDelete(nullptr);
}

size_t json_array_size(const cJSON* root, const char* table) {
  const cJSON* dictionary = cJSON_GetObjectItemCaseSensitive(root, "dictionary");
  return static_cast<size_t>(
      cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(dictionary, table)));
}

void test_mutex_constructed_during_export_gets_a_semaphore() {
  MutexConstructor constructor;
  constructor.owner = xTaskGetCurrentTaskHandle();
  xTaskCreatePinnedToCore(construct_mutexes, "mutex_ctor", 4096, &constructor,
                          uxTaskPriorityGet(nullptr), nullptr, 1 - xPortGetCoreID());
  while (constructor.constructed.load() == 0) {
    vTaskDelay(1);
  }

  export_worst_case();
  constructor.stop.store(true);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

  printf("mutexes constructed during the export: %u, without a semaphore: %u\n",
         static_cast<unsigned>(constructor.constructed.load()),
         static_cast<unsigned>(constructor.without_semaphore.load()));
  TEST_ASSERT_EQUAL_UINT32(0, constructor.without_semaphore.load());
}

void test_export_document_is_json() {
  export_worst_case();
  cJSON* root = cJSON_Parse(g_body);
  TEST_ASSERT_NOT_NULL(root);
  cJSON_Delete(root);
}

void test_export_writes_one_stack_per_aggregate() {
  export_worst_case();
  cJSON* root = cJSON_Parse(g_body);
  const size_t stacks = json_array_size(root, "stackTable");
  cJSON_Delete(root);
  TEST_ASSERT_EQUAL_UINT32(kStacks, stacks);
}

void test_export_writes_one_location_per_distinct_address() {
  export_worst_case();
  cJSON* root = cJSON_Parse(g_body);
  const size_t locations = json_array_size(root, "locationTable");
  cJSON_Delete(root);
  TEST_ASSERT_EQUAL_UINT32(kStacks * kDepth, locations);
}

}  // namespace

void setUp() {}
void tearDown() {}

extern "C" void app_main() {
  esp_opentelemetry::set_profiles_exporter(std::make_unique<CapturingExporter>());
  make_worst_case_stacks();
  // The first export also initialises function-local statics, whose guards
  // take a FreeRTOS mutex of their own; doing it here keeps the tests on the
  // steady state every later export runs in.
  esp_opentelemetry::export_profiles(g_stacks, 1, kTimeUnixNano, kDurationNano);

  UNITY_BEGIN();
  RUN_TEST(test_mutex_constructed_during_export_gets_a_semaphore);
  RUN_TEST(test_export_document_is_json);
  RUN_TEST(test_export_writes_one_stack_per_aggregate);
  RUN_TEST(test_export_writes_one_location_per_distinct_address);
  UNITY_END();
}
