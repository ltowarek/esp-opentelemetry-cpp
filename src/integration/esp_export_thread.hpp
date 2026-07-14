// Shared pthread configuration for the SDK's exporter background threads.
//
// The OTLP export call chains (DoBackgroundWork -> exporter -> protobuf arena
// -> mbedTLS) are ~15 frames deep and overflow the 32 KB default pthread
// stack, so any thread the SDK spawns during provider construction gets a
// 64 KB PSRAM stack. RAII: construct before creating the processor/reader,
// destroy to restore the default config.

#pragma once

extern "C" {
#include "esp_heap_caps.h"
#include "esp_pthread.h"
}

namespace esp_opentelemetry {

class ScopedExportThreadConfig {
 public:
  // prio < 0 leaves the default thread priority untouched.
  explicit ScopedExportThreadConfig(int prio = -1)
      : orig_(esp_pthread_get_default_config()) {
    esp_pthread_cfg_t cfg = orig_;
    cfg.stack_size = 65536;
    cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    if (prio >= 0) {
      cfg.prio = prio;
    }
    esp_pthread_set_cfg(&cfg);
  }

  ~ScopedExportThreadConfig() { esp_pthread_set_cfg(&orig_); }

  ScopedExportThreadConfig(const ScopedExportThreadConfig&) = delete;
  ScopedExportThreadConfig& operator=(const ScopedExportThreadConfig&) = delete;

 private:
  esp_pthread_cfg_t orig_;
};

}  // namespace esp_opentelemetry
