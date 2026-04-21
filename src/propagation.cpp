#include "propagation.hpp"

#include "opentelemetry/context/runtime_context.h"
#include "opentelemetry/context/propagation/global_propagator.h"
#include "opentelemetry/trace/propagation/http_trace_context.h"

namespace esp_opentelemetry {

opentelemetry::nostd::string_view CJsonCarrier::Get(
    opentelemetry::nostd::string_view key) const noexcept {
  if (obj_ == nullptr) {
    return {};
  }
  // cJSON keys are NUL-terminated; fold the string_view into a temporary
  // std::string for the C API.
  std::string k(key.data(), key.size());
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj_, k.c_str());
  if (item == nullptr || !cJSON_IsString(item) ||
      item->valuestring == nullptr) {
    return {};
  }
  return opentelemetry::nostd::string_view(item->valuestring);
}

void CJsonCarrier::Set(opentelemetry::nostd::string_view key,
                       opentelemetry::nostd::string_view value) noexcept {
  if (obj_ == nullptr) {
    return;
  }
  std::string k(key.data(), key.size());
  std::string v(value.data(), value.size());
  cJSON_DeleteItemFromObjectCaseSensitive(obj_, k.c_str());
  cJSON_AddStringToObject(obj_, k.c_str(), v.c_str());
}

void inject_traceparent(cJSON* obj) {
  if (obj == nullptr) {
    return;
  }
  auto propagator =
      opentelemetry::context::propagation::GlobalTextMapPropagator::
          GetGlobalPropagator();
  if (!propagator) {
    return;
  }
  CJsonCarrier carrier(obj);
  auto ctx = opentelemetry::context::RuntimeContext::GetCurrent();
  propagator->Inject(carrier, ctx);
}

opentelemetry::context::Context extract_traceparent(const cJSON* obj) {
  auto current = opentelemetry::context::RuntimeContext::GetCurrent();
  if (obj == nullptr) {
    return current;
  }
  auto propagator =
      opentelemetry::context::propagation::GlobalTextMapPropagator::
          GetGlobalPropagator();
  if (!propagator) {
    return current;
  }
  // cJSON APIs do not take const - the carrier only reads, but we need a
  // non-const pointer for cJSON_GetObjectItemCaseSensitive.
  CJsonCarrier carrier(const_cast<cJSON*>(obj));
  return propagator->Extract(carrier, current);
}

}  // namespace esp_opentelemetry
