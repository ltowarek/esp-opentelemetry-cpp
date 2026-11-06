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
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>

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

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

Json parse_body() { return Json(cJSON_Parse(g_body), &cJSON_Delete); }

Json parse_worst_case_export() {
  export_worst_case();
  return parse_body();
}

Json export_task(const char* task_name) {
  esp_opentelemetry::ProfileStack stack = g_stacks[0];
  stack.task_name = task_name;
  heap_caps_free(g_body);
  g_body = nullptr;
  esp_opentelemetry::export_profiles(&stack, 1, kTimeUnixNano, kDurationNano);
  return parse_body();
}

// The document holds one stack, so its only thread_name attribute is that stack's.
std::string exported_thread_name(const cJSON* root) {
  const cJSON* dictionary = cJSON_GetObjectItemCaseSensitive(root, "dictionary");
  const cJSON* strings = cJSON_GetObjectItemCaseSensitive(dictionary, "stringTable");
  const cJSON* attribute = nullptr;
  cJSON_ArrayForEach(attribute, cJSON_GetObjectItemCaseSensitive(dictionary, "attributeTable")) {
    const int key = cJSON_GetObjectItemCaseSensitive(attribute, "keyStrindex")->valueint;
    if (std::strcmp(cJSON_GetArrayItem(strings, key)->valuestring, "thread_name") == 0) {
      const cJSON* value = cJSON_GetObjectItemCaseSensitive(attribute, "value");
      return cJSON_GetObjectItemCaseSensitive(value, "stringValue")->valuestring;
    }
  }
  return "";
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

  TEST_ASSERT_EQUAL_UINT32(0, constructor.without_semaphore.load());
}

void test_export_document_is_json() {
  TEST_ASSERT_NOT_NULL(parse_worst_case_export().get());
}

void test_export_writes_one_stack_per_aggregate() {
  TEST_ASSERT_EQUAL_UINT32(kStacks, json_array_size(parse_worst_case_export().get(), "stackTable"));
}

void test_export_writes_one_location_per_distinct_address() {
  TEST_ASSERT_EQUAL_UINT32(kStacks * kDepth,
                           json_array_size(parse_worst_case_export().get(), "locationTable"));
}

// FreeRTOS cuts a task name at CONFIG_FREERTOS_MAX_TASK_NAME_LEN - 1 bytes,
// which can fall inside a multi-byte character.
void test_task_name_cut_mid_character_keeps_its_whole_characters() {
  TEST_ASSERT_EQUAL_STRING(
      "w\xc4\x85tek_kamery_",
      exported_thread_name(export_task("w\xc4\x85tek_kamery_\xc5").get()).c_str());
}

void test_task_name_ending_in_a_whole_multibyte_character_is_unchanged() {
  TEST_ASSERT_EQUAL_STRING("kamera_\xc5\x82",
                           exported_thread_name(export_task("kamera_\xc5\x82").get()).c_str());
}

// Task names are arbitrary bytes; JSON strings must be UTF-8.
void test_task_name_byte_that_is_not_utf8_becomes_a_replacement_character() {
  TEST_ASSERT_EQUAL_STRING("kamera_\xc5\x82\xef\xbf\xbd",
                           exported_thread_name(export_task("kamera_\xc5\x82\xff").get()).c_str());
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
  RUN_TEST(test_task_name_cut_mid_character_keeps_its_whole_characters);
  RUN_TEST(test_task_name_ending_in_a_whole_multibyte_character_is_unchanged);
  RUN_TEST(test_task_name_byte_that_is_not_utf8_becomes_a_replacement_character);
  UNITY_END();
}
