// Routes a translation unit's existing ESP_LOGE/ESP_LOGW/ESP_LOGI call sites
// through the OpenTelemetry logger installed by
// esp_opentelemetry_logs_setup(), in addition to the usual console output.
//
// Usage — include after esp_log.h, in each .c/.cpp that should ship its logs:
//
//   #include "esp_log.h"
//   #include "esp_log_otel.h"
//
//   ESP_LOGI(TAG, "connected to %s", ssid);   // unchanged; now also exported
//
// The wrappers add the call site's file, line and function, captured at
// macro-expansion time — the same source-capture idiom the SDK's own
// OTEL_INTERNAL_LOG_* macros use. They are gated on ESP_LOG_ENABLED(), the
// project's own LOG_LOCAL_LEVEL / CONFIG_LOG_MAXIMUM_LEVEL compile-time cap,
// so a level compiled out of the console log is never exported either. Runtime
// filtering is not shared: esp_log_level_set() still suppresses the console
// line, but the record is exported regardless.
//
// Each call expands to a single bridge call that formats the message once and
// feeds both sinks, so arguments are evaluated exactly once — as they are with
// the stock macros. The cost is that a message longer than
// CONFIG_ESP_OPENTELEMETRY_LOGS_MAX_BODY_LEN is truncated on the console too,
// not just in the record.
//
// Deliberately not included by esp_opentelemetry.hpp: the component's own
// translation units (the HTTP transport, the exporters) log from inside the
// export path, and redefining ESP_LOGx there would feed every failed export
// back into the queue that produced it.
//
// When CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED is off the header defines
// nothing: including it leaves ESP-IDF's own macros in place, so a consumer
// can include it unconditionally.
//
// Only the E/W/I levels are wrapped. D/V are the levels projects compile out
// of production images, and exporting them would multiply the export volume
// the batch queue is sized for.

#pragma once

#include "esp_log.h"
#include "sdkconfig.h"

#ifdef CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED

#include "esp_log_otel_emit.h"

// ESP_LOG_GET_LEVEL strips the non-level bits ESP-IDF's log v2 packs into the
// same argument. It masks to an int, which C++ will not narrow to the enum on
// its own.
#define ESP_OTEL_LEVEL(level) ((esp_log_level_t)ESP_LOG_GET_LEVEL(level))

// ESP-IDF switches between __VA_OPT__ and the ##__VA_ARGS__ GNU extension on
// this condition; mirror it so the wrappers accept the same call sites the
// macros they replace do.
#if defined(__cplusplus) && (__cplusplus > 201703L)

#define ESP_OTEL_LOG_LEVEL_LOCAL(level, tag, format, ...)                     \
  do {                                                                        \
    if (ESP_LOG_ENABLED(level)) {                                             \
      esp_opentelemetry_log_and_emit(ESP_OTEL_LEVEL(level), tag, __FILE__,     \
                                     __LINE__, __func__,                      \
                                     format __VA_OPT__(, ) __VA_ARGS__);      \
    }                                                                         \
  } while (0)

#else  // !(defined(__cplusplus) && (__cplusplus > 201703L))

#define ESP_OTEL_LOG_LEVEL_LOCAL(level, tag, format, ...)                     \
  do {                                                                        \
    if (ESP_LOG_ENABLED(level)) {                                             \
      esp_opentelemetry_log_and_emit(ESP_OTEL_LEVEL(level), tag, __FILE__,     \
                                     __LINE__, __func__, format,              \
                                     ##__VA_ARGS__);                          \
    }                                                                         \
  } while (0)

#endif  // !(defined(__cplusplus) && (__cplusplus > 201703L))

#undef ESP_LOGE
#undef ESP_LOGW
#undef ESP_LOGI

#if defined(__cplusplus) && (__cplusplus > 201703L)
#define ESP_LOGE(tag, format, ...) \
  ESP_OTEL_LOG_LEVEL_LOCAL(ESP_LOG_ERROR, tag, format __VA_OPT__(, ) __VA_ARGS__)
#define ESP_LOGW(tag, format, ...) \
  ESP_OTEL_LOG_LEVEL_LOCAL(ESP_LOG_WARN, tag, format __VA_OPT__(, ) __VA_ARGS__)
#define ESP_LOGI(tag, format, ...) \
  ESP_OTEL_LOG_LEVEL_LOCAL(ESP_LOG_INFO, tag, format __VA_OPT__(, ) __VA_ARGS__)
#else
#define ESP_LOGE(tag, format, ...) \
  ESP_OTEL_LOG_LEVEL_LOCAL(ESP_LOG_ERROR, tag, format, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) \
  ESP_OTEL_LOG_LEVEL_LOCAL(ESP_LOG_WARN, tag, format, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) \
  ESP_OTEL_LOG_LEVEL_LOCAL(ESP_LOG_INFO, tag, format, ##__VA_ARGS__)
#endif

#endif  // CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED
