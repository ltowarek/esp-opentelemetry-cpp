// Turning a signal off is a configuration change only: the application builds
// every signal's exporter from its base URL and hands it to the signal's setup
// call, the same code whichever signals are enabled. With a signal disabled,
// its base URL still exists, its factories return nullptr, and its setup call
// leaves it off.

#include "esp_jtag_exporters.hpp"
#include "esp_log_exporters.hpp"
#include "esp_opentelemetry.hpp"
#include "esp_otlp_http_exporters.hpp"

extern "C" {
#include "unity.h"
}

#include <utility>

namespace {

void test_otlp_http_span_exporter_is_null() {
  TEST_ASSERT_NULL(
      esp_opentelemetry::MakeOtlpHttpSpanExporter(CONFIG_ESP_OPENTELEMETRY_TRACING_OTLP_BASE_URL)
          .get());
}

void test_otlp_http_metric_exporter_is_null() {
  TEST_ASSERT_NULL(
      esp_opentelemetry::MakeOtlpHttpMetricExporter(CONFIG_ESP_OPENTELEMETRY_METRICS_OTLP_BASE_URL)
          .get());
}

void test_otlp_http_log_record_exporter_is_null() {
  TEST_ASSERT_NULL(
      esp_opentelemetry::MakeOtlpHttpLogRecordExporter(CONFIG_ESP_OPENTELEMETRY_LOGS_OTLP_BASE_URL)
          .get());
}

void test_jtag_span_exporter_is_null() {
  TEST_ASSERT_NULL(esp_opentelemetry::MakeJtagSpanExporter().get());
}

void test_jtag_metric_exporter_is_null() {
  TEST_ASSERT_NULL(esp_opentelemetry::MakeJtagMetricExporter().get());
}

void test_jtag_log_record_exporter_is_null() {
  TEST_ASSERT_NULL(esp_opentelemetry::MakeJtagLogRecordExporter().get());
}

void test_esp_log_span_exporter_is_null() {
  TEST_ASSERT_NULL(esp_opentelemetry::MakeEspLogSpanExporter().get());
}

void test_esp_log_metric_exporter_is_null() {
  TEST_ASSERT_NULL(esp_opentelemetry::MakeEspLogMetricExporter().get());
}

void test_esp_log_log_record_exporter_is_null() {
  TEST_ASSERT_NULL(esp_opentelemetry::MakeEspLogLogRecordExporter().get());
}

}  // namespace

void setUp() {}
void tearDown() {}

extern "C" void app_main() {
  auto span_exporter =
      esp_opentelemetry::MakeOtlpHttpSpanExporter(CONFIG_ESP_OPENTELEMETRY_TRACING_OTLP_BASE_URL);
  auto metric_exporter =
      esp_opentelemetry::MakeOtlpHttpMetricExporter(CONFIG_ESP_OPENTELEMETRY_METRICS_OTLP_BASE_URL);
  auto log_exporter =
      esp_opentelemetry::MakeOtlpHttpLogRecordExporter(CONFIG_ESP_OPENTELEMETRY_LOGS_OTLP_BASE_URL);
  auto profiles_exporter = esp_opentelemetry::MakeOtlpHttpProfilesExporter(
      CONFIG_ESP_OPENTELEMETRY_PROFILES_OTLP_BASE_URL);
  esp_opentelemetry_tracing_setup(std::move(span_exporter));
  esp_opentelemetry_metrics_setup(std::move(metric_exporter));
  esp_opentelemetry_logs_setup(std::move(log_exporter));
  esp_opentelemetry_profiling_setup(std::move(profiles_exporter));

  UNITY_BEGIN();
  RUN_TEST(test_otlp_http_span_exporter_is_null);
  RUN_TEST(test_otlp_http_metric_exporter_is_null);
  RUN_TEST(test_otlp_http_log_record_exporter_is_null);
  RUN_TEST(test_jtag_span_exporter_is_null);
  RUN_TEST(test_jtag_metric_exporter_is_null);
  RUN_TEST(test_jtag_log_record_exporter_is_null);
  RUN_TEST(test_esp_log_span_exporter_is_null);
  RUN_TEST(test_esp_log_metric_exporter_is_null);
  RUN_TEST(test_esp_log_log_record_exporter_is_null);
  UNITY_END();
}
