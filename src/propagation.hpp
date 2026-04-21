// Internal helpers for injecting/extracting W3C traceparent into cJSON
// payloads. Declared here (not in esp_opentelemetry.hpp) because the
// inject/extract entry points in esp_opentelemetry.hpp are the public C++
// API; these live in a private header so the tests can exercise the
// TextMapCarrier in isolation.

#pragma once

#include "opentelemetry/context/context.h"
#include "opentelemetry/context/propagation/text_map_propagator.h"

#include <cJSON.h>
#include <string>

namespace esp_opentelemetry {

// TextMapCarrier reading/writing "traceparent" and "tracestate" keys on a
// cJSON object. Matches the shape the Python TraceContextTextMapPropagator
// expects so the generated header is byte-identical to what the
// controller emits.
class CJsonCarrier
    : public opentelemetry::context::propagation::TextMapCarrier {
public:
  explicit CJsonCarrier(cJSON* obj) : obj_(obj) {}

  opentelemetry::nostd::string_view Get(
      opentelemetry::nostd::string_view key) const noexcept override;

  void Set(opentelemetry::nostd::string_view key,
           opentelemetry::nostd::string_view value) noexcept override;

private:
  cJSON* obj_;
};

void inject_traceparent(cJSON* obj);

opentelemetry::context::Context extract_traceparent(const cJSON* obj);

}  // namespace esp_opentelemetry
