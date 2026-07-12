// No-op definitions of the public profiling API for builds with
// CONFIG_ESP_OPENTELEMETRY_PROFILING_ENABLED off (see CMakeLists.txt, which
// compiles this instead of esp_profiling.cpp/esp_profiles_exporter.cpp in
// that case) — callers (e.g. app_main()) call these unconditionally.

#include "esp_profiling.hpp"

void esp_opentelemetry_profiling_setup() {}

uint32_t esp_opentelemetry_profiling_samples() { return 0; }

uint32_t esp_opentelemetry_profiling_spanned_samples() { return 0; }
