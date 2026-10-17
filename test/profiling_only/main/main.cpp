// Profiling needs none of the opentelemetry-cpp SDK, so a build with it the
// only signal on compiles no SDK while every exporter family still has to
// provide a working profiles factory.

#include "esp_jtag_exporters.hpp"
#include "esp_log_exporters.hpp"
#include "esp_otlp_http_exporters.hpp"
#include "esp_profiles_exporter.hpp"

extern "C" {
#include "unity.h"
}

namespace {

void test_otlp_http_profiles_exporter_is_built() {
  TEST_ASSERT_NOT_NULL(esp_opentelemetry::MakeOtlpHttpProfilesExporter(
                           CONFIG_ESP_OPENTELEMETRY_PROFILES_OTLP_BASE_URL)
                           .get());
}

void test_jtag_profiles_exporter_is_built() {
  TEST_ASSERT_NOT_NULL(esp_opentelemetry::MakeJtagProfilesExporter().get());
}

void test_esp_log_profiles_exporter_is_built() {
  TEST_ASSERT_NOT_NULL(esp_opentelemetry::MakeEspLogProfilesExporter().get());
}

void test_ostream_profiles_exporter_is_built() {
  TEST_ASSERT_NOT_NULL(esp_opentelemetry::MakeOstreamProfilesExporter().get());
}

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

}  // namespace

void setUp() {}
void tearDown() {}

extern "C" void app_main() {
  UNITY_BEGIN();
  RUN_TEST(test_otlp_http_profiles_exporter_is_built);
  RUN_TEST(test_jtag_profiles_exporter_is_built);
  RUN_TEST(test_esp_log_profiles_exporter_is_built);
  RUN_TEST(test_ostream_profiles_exporter_is_built);
  RUN_TEST(test_otlp_http_span_exporter_is_null);
  RUN_TEST(test_otlp_http_metric_exporter_is_null);
  RUN_TEST(test_otlp_http_log_record_exporter_is_null);
  UNITY_END();
}
