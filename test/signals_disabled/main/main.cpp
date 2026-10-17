// Turning a signal off is a configuration change only: the application builds
// every signal's exporter from its base URL and hands it to the signal's setup
// call, the same code whichever signals are enabled. With a signal disabled,
// its base URL still exists, its factories return nullptr, and its setup call
// leaves it off.

#include "esp_jtag_exporters.hpp"
#include "esp_log_exporters.hpp"
#include "esp_opentelemetry.hpp"
#include "esp_otlp_http_exporters.hpp"
#include "esp_profiles_exporter.hpp"

#include "opentelemetry/logs/provider.h"
#include "opentelemetry/metrics/provider.h"
#include "opentelemetry/trace/provider.h"

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
}

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

void test_otlp_http_profiles_exporter_is_null() {
  TEST_ASSERT_NULL(esp_opentelemetry::MakeOtlpHttpProfilesExporter(
                       CONFIG_ESP_OPENTELEMETRY_PROFILES_OTLP_BASE_URL)
                       .get());
}

void test_jtag_profiles_exporter_is_null() {
  TEST_ASSERT_NULL(esp_opentelemetry::MakeJtagProfilesExporter().get());
}

void test_esp_log_profiles_exporter_is_null() {
  TEST_ASSERT_NULL(esp_opentelemetry::MakeEspLogProfilesExporter().get());
}

void test_ostream_profiles_exporter_is_null() {
  TEST_ASSERT_NULL(esp_opentelemetry::MakeOstreamProfilesExporter().get());
}

void test_tracing_setup_keeps_the_noop_tracer_provider() {
  const auto before = opentelemetry::trace::Provider::GetTracerProvider();
  esp_opentelemetry_tracing_setup(
      esp_opentelemetry::MakeOtlpHttpSpanExporter(CONFIG_ESP_OPENTELEMETRY_TRACING_OTLP_BASE_URL));
  TEST_ASSERT_EQUAL_PTR(before.get(), opentelemetry::trace::Provider::GetTracerProvider().get());
}

void test_metrics_setup_keeps_the_noop_meter_provider() {
  const auto before = opentelemetry::metrics::Provider::GetMeterProvider();
  esp_opentelemetry_metrics_setup(
      esp_opentelemetry::MakeOtlpHttpMetricExporter(CONFIG_ESP_OPENTELEMETRY_METRICS_OTLP_BASE_URL));
  TEST_ASSERT_EQUAL_PTR(before.get(), opentelemetry::metrics::Provider::GetMeterProvider().get());
}

void test_logs_setup_keeps_the_noop_logger_provider() {
  const auto before = opentelemetry::logs::Provider::GetLoggerProvider();
  esp_opentelemetry_logs_setup(
      esp_opentelemetry::MakeOtlpHttpLogRecordExporter(CONFIG_ESP_OPENTELEMETRY_LOGS_OTLP_BASE_URL));
  TEST_ASSERT_EQUAL_PTR(before.get(), opentelemetry::logs::Provider::GetLoggerProvider().get());
}

void test_tracer_records_no_span() {
  TEST_ASSERT_FALSE(esp_opentelemetry_tracer()->StartSpan("span")->IsRecording());
}

void test_logger_is_disabled() {
  TEST_ASSERT_FALSE(esp_opentelemetry_logger()->Enabled(opentelemetry::logs::Severity::kFatal));
}

void test_profiling_setup_takes_no_samples() {
  esp_opentelemetry_profiling_setup(esp_opentelemetry::MakeOtlpHttpProfilesExporter(
      CONFIG_ESP_OPENTELEMETRY_PROFILES_OTLP_BASE_URL));
  vTaskDelay(pdMS_TO_TICKS(100));
  TEST_ASSERT_EQUAL_UINT32(0, esp_opentelemetry_profiling_samples());
}

}  // namespace

void setUp() {}
void tearDown() {}

extern "C" void app_main() {
  UNITY_BEGIN();
  RUN_TEST(test_otlp_http_span_exporter_is_null);
  RUN_TEST(test_otlp_http_metric_exporter_is_null);
  RUN_TEST(test_otlp_http_log_record_exporter_is_null);
  RUN_TEST(test_otlp_http_profiles_exporter_is_null);
  RUN_TEST(test_jtag_span_exporter_is_null);
  RUN_TEST(test_jtag_metric_exporter_is_null);
  RUN_TEST(test_jtag_log_record_exporter_is_null);
  RUN_TEST(test_jtag_profiles_exporter_is_null);
  RUN_TEST(test_esp_log_span_exporter_is_null);
  RUN_TEST(test_esp_log_metric_exporter_is_null);
  RUN_TEST(test_esp_log_log_record_exporter_is_null);
  RUN_TEST(test_esp_log_profiles_exporter_is_null);
  RUN_TEST(test_ostream_profiles_exporter_is_null);
  RUN_TEST(test_tracing_setup_keeps_the_noop_tracer_provider);
  RUN_TEST(test_metrics_setup_keeps_the_noop_meter_provider);
  RUN_TEST(test_logs_setup_keeps_the_noop_logger_provider);
  RUN_TEST(test_tracer_records_no_span);
  RUN_TEST(test_logger_is_disabled);
  RUN_TEST(test_profiling_setup_takes_no_samples);
  UNITY_END();
}
