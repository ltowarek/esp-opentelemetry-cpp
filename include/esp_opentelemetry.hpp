// OpenTelemetry facade for ESP-IDF firmware.
//
// This header is the single include for all signals (traces, metrics, logs,
// profiles) — it pulls in esp_tracing.hpp / esp_metrics.hpp / esp_logs.hpp /
// esp_profiling.hpp, each declaring one signal's API. Include just the
// signal-specific header instead if you only want that signal's declarations.
//
// It deliberately does not pull in esp_log_otel.h: that header redefines
// ESP_LOGx, which is opt-in per translation unit.

#pragma once

#include "esp_tracing.hpp"
#include "esp_metrics.hpp"
#include "esp_logs.hpp"
#include "esp_profiling.hpp"
