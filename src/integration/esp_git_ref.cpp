#include "esp_git_ref.hpp"

extern "C" {
#include "esp_app_desc.h"
}

#include <string>
#include <string_view>

namespace esp_opentelemetry {

const char* current_git_ref() {
  static const std::string ref = [] {
    std::string v = esp_app_get_description()->version;
    constexpr std::string_view kDirtySuffix = "-dirty";
    if (v.size() > kDirtySuffix.size() &&
        v.compare(v.size() - kDirtySuffix.size(), kDirtySuffix.size(), kDirtySuffix) == 0) {
      v.resize(v.size() - kDirtySuffix.size());
    }
    return v;
  }();
  return ref.c_str();
}

}  // namespace esp_opentelemetry
