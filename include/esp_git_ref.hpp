#pragma once

namespace esp_opentelemetry {

// Git ref for the running firmware, from esp_app_desc_t.version with a
// trailing "-dirty" stripped. Cached after the first call.
const char* current_git_ref();

}  // namespace esp_opentelemetry
