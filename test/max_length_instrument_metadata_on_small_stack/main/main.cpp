// An instrument's name and unit are validated on whichever task creates it,
// and an application may do that on a task with only a few KB of stack. The
// validation must fit there for every input, including the longest name and
// unit the specification allows, and must accept exactly what the
// specification's patterns accept.

#include <chrono>
#include <memory>
#include <string>

#include "opentelemetry/metrics/meter.h"
#include "opentelemetry/metrics/sync_instruments.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/nostd/unique_ptr.h"
#include "opentelemetry/sdk/metrics/export/metric_producer.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/meter_provider.h"
#include "opentelemetry/sdk/metrics/metric_reader.h"

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
}

namespace {

namespace metrics_api = opentelemetry::metrics;
namespace metrics_sdk = opentelemetry::sdk::metrics;
namespace nostd = opentelemetry::nostd;

constexpr uint32_t kSmallStackBytes = 4096;
constexpr uint32_t kMinStackHeadroomBytes = 1024;
constexpr TickType_t kCreationTimeout = pdMS_TO_TICKS(5000);

const std::string kMaxLengthName = "n" + std::string(254, 'a');
const std::string kMaxLengthUnit(63, 'u');

class CollectingReader : public metrics_sdk::MetricReader {
 public:
  metrics_sdk::AggregationTemporality GetAggregationTemporality(
      metrics_sdk::InstrumentType /*instrument_type*/) const noexcept override {
    return metrics_sdk::AggregationTemporality::kCumulative;
  }

 private:
  bool OnForceFlush(std::chrono::microseconds /*timeout*/) noexcept override { return true; }
  bool OnShutDown(std::chrono::microseconds /*timeout*/) noexcept override { return true; }
};

bool ContainsMetricNamed(const metrics_sdk::ResourceMetrics &metrics, const std::string &name) {
  for (const auto &scope : metrics.scope_metric_data_) {
    for (const auto &metric : scope.metric_data_) {
      if (metric.instrument_descriptor.name_ == name) {
        return true;
      }
    }
  }
  return false;
}

bool HasMetricNamed(CollectingReader &reader, const std::string &name) {
  bool found = false;
  reader.Collect([&](metrics_sdk::ResourceMetrics &metrics) {
    found = ContainsMetricNamed(metrics, name);
    return true;
  });
  return found;
}

std::shared_ptr<CollectingReader> AddCollectingReader(metrics_sdk::MeterProvider &provider) {
  auto reader = std::make_shared<CollectingReader>();
  provider.AddMetricReader(reader);
  return reader;
}

bool IsAccepted(const std::string &name, const std::string &unit) {
  metrics_sdk::MeterProvider provider;
  auto reader = AddCollectingReader(provider);
  provider.GetMeter("test")->CreateUInt64Counter(name, "", unit)->Add(1);
  return HasMetricNamed(*reader, name);
}

struct SmallStackCreation {
  nostd::shared_ptr<metrics_api::Meter> meter;
  TaskHandle_t creator;
  nostd::unique_ptr<metrics_api::Counter<uint64_t>> counter;
  UBaseType_t stack_high_water_mark_bytes;
};

void CreateMaxLengthCounter(void *arg) {
  auto *creation = static_cast<SmallStackCreation *>(arg);
  creation->counter = creation->meter->CreateUInt64Counter(kMaxLengthName, "", kMaxLengthUnit);
  creation->stack_high_water_mark_bytes = uxTaskGetStackHighWaterMark(nullptr);
  xTaskNotifyGive(creation->creator);
  vTaskDelete(nullptr);
}

void CreateMaxLengthCounterOnSmallStack(SmallStackCreation &creation) {
  TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(CreateMaxLengthCounter, "small_stack", kSmallStackBytes,
                                        &creation, uxTaskPriorityGet(nullptr), nullptr));
  TEST_ASSERT_EQUAL_UINT32(1, ulTaskNotifyTake(pdTRUE, kCreationTimeout));
}

void test_max_length_counter_leaves_stack_headroom_on_a_4_kb_task() {
  metrics_sdk::MeterProvider provider;
  SmallStackCreation creation{provider.GetMeter("test"), xTaskGetCurrentTaskHandle(), nullptr, 0};
  CreateMaxLengthCounterOnSmallStack(creation);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(kMinStackHeadroomBytes, creation.stack_high_water_mark_bytes);
}

void test_max_length_counter_created_on_a_4_kb_task_is_recorded() {
  metrics_sdk::MeterProvider provider;
  auto reader = AddCollectingReader(provider);
  SmallStackCreation creation{provider.GetMeter("test"), xTaskGetCurrentTaskHandle(), nullptr, 0};
  CreateMaxLengthCounterOnSmallStack(creation);
  creation.counter->Add(1);
  TEST_ASSERT_TRUE(HasMetricNamed(*reader, kMaxLengthName));
}

void test_single_letter_name_is_accepted() { TEST_ASSERT_TRUE(IsAccepted("a", "")); }

void test_name_with_every_allowed_punctuation_and_digit_is_accepted() {
  TEST_ASSERT_TRUE(IsAccepted("Az-_./09", ""));
}

void test_empty_name_is_rejected() { TEST_ASSERT_FALSE(IsAccepted("", "")); }

void test_name_longer_than_255_chars_is_rejected() {
  TEST_ASSERT_FALSE(IsAccepted(kMaxLengthName + "a", ""));
}

void test_name_starting_with_a_digit_is_rejected() { TEST_ASSERT_FALSE(IsAccepted("1a", "")); }

void test_name_starting_with_an_underscore_is_rejected() {
  TEST_ASSERT_FALSE(IsAccepted("_a", ""));
}

void test_name_with_a_space_is_rejected() { TEST_ASSERT_FALSE(IsAccepted("a b", "")); }

void test_name_with_a_non_ascii_letter_is_rejected() {
  TEST_ASSERT_FALSE(IsAccepted("a\xC3\xA9", ""));
}

void test_name_with_an_embedded_nul_is_rejected() {
  TEST_ASSERT_FALSE(IsAccepted(std::string("a\0b", 3), ""));
}

void test_unit_longer_than_63_chars_is_rejected() {
  TEST_ASSERT_FALSE(IsAccepted("a", kMaxLengthUnit + "u"));
}

void test_unit_with_a_non_ascii_char_is_rejected() {
  TEST_ASSERT_FALSE(IsAccepted("a", "\xC2\xB0"
                                    "C"));
}

void test_unit_with_an_embedded_nul_is_rejected() {
  TEST_ASSERT_FALSE(IsAccepted("a", std::string("m\0s", 3)));
}

}  // namespace

void setUp() {}
void tearDown() {}

extern "C" void app_main() {
  UNITY_BEGIN();
  RUN_TEST(test_max_length_counter_leaves_stack_headroom_on_a_4_kb_task);
  RUN_TEST(test_max_length_counter_created_on_a_4_kb_task_is_recorded);
  RUN_TEST(test_single_letter_name_is_accepted);
  RUN_TEST(test_name_with_every_allowed_punctuation_and_digit_is_accepted);
  RUN_TEST(test_empty_name_is_rejected);
  RUN_TEST(test_name_longer_than_255_chars_is_rejected);
  RUN_TEST(test_name_starting_with_a_digit_is_rejected);
  RUN_TEST(test_name_starting_with_an_underscore_is_rejected);
  RUN_TEST(test_name_with_a_space_is_rejected);
  RUN_TEST(test_name_with_a_non_ascii_letter_is_rejected);
  RUN_TEST(test_name_with_an_embedded_nul_is_rejected);
  RUN_TEST(test_unit_longer_than_63_chars_is_rejected);
  RUN_TEST(test_unit_with_a_non_ascii_char_is_rejected);
  RUN_TEST(test_unit_with_an_embedded_nul_is_rejected);
  UNITY_END();
}
