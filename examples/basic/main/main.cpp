#include "esp_opentelemetry.hpp"

extern "C" void app_main() {
  esp_opentelemetry_setup("basic-example");
  auto tracer = esp_opentelemetry_tracer();
  auto span = tracer->StartSpan("app_main");
  span->End();
}
