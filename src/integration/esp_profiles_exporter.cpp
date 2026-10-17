#include "esp_profiles_exporter.hpp"

#if defined(CONFIG_ESP_OPENTELEMETRY_EXPORTER_OSTREAM)

#include <cstdio>

namespace esp_opentelemetry {

#if defined(CONFIG_ESP_OPENTELEMETRY_PROFILING_ENABLED)

namespace {

class OstreamProfilesExporter final : public ProfilesExporter {
 public:
  bool Export(const char* body, std::size_t /*size*/) noexcept override {
    printf("PROFILE_JSON_BEGIN\n%s\nPROFILE_JSON_END\n", body);
    return true;
  }
};

}  // namespace

std::unique_ptr<ProfilesExporter> MakeOstreamProfilesExporter() {
  return std::make_unique<OstreamProfilesExporter>();
}

#else

std::unique_ptr<ProfilesExporter> MakeOstreamProfilesExporter() {
  return nullptr;
}

#endif  // CONFIG_ESP_OPENTELEMETRY_PROFILING_ENABLED

}  // namespace esp_opentelemetry

#endif  // CONFIG_ESP_OPENTELEMETRY_EXPORTER_OSTREAM
