// A translation unit that lowers the compile-time log level for itself, the
// way a noisy module in a real project would. LOG_LOCAL_LEVEL must be set
// before esp_log.h is first included.
//
// The ESP_LOGx wrappers are gated on the same ESP_LOG_ENABLED() check the
// stock macros use, so the ESP_LOGI below reaches neither the console nor the
// exporter, while the ESP_LOGW does both.

#define LOG_LOCAL_LEVEL ESP_LOG_WARN

#include "esp_log.h"
#include "esp_log_otel.h"

#include "capped.hpp"

namespace {
constexpr const char* TAG = "capped";
}

void log_from_capped_module() {
  ESP_LOGI(TAG, "info below this module's cap");
  ESP_LOGW(TAG, "warn above this module's cap");
}
