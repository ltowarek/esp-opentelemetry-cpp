// The bridge function esp_log_otel.h's ESP_LOGx wrappers expand to.
//
// Separate from esp_log_otel.h so a translation unit can reach the bridge
// without taking the ESP_LOGx redefinition — which is what the component's
// own sources do, since they log from inside the export path.

#pragma once

#include "esp_log_level.h"

#ifdef __cplusplus
extern "C" {
#endif

// Write one ESP_LOG line to the console and emit it as a log record.
//
// The message is printf-formatted once into a
// CONFIG_ESP_OPENTELEMETRY_LOGS_MAX_BODY_LEN buffer and truncated if it does
// not fit, then handed to both sinks — so the two never disagree and the
// caller's arguments are evaluated exactly once. The buffer lives in this
// function's frame, not the caller's.
//
// The console line is written through ESP-IDF's own ESP_LOGE/W/I with the
// caller's tag, so decoration, timestamps and runtime tag filtering
// (esp_log_level_set) behave as they always have. The record, in contrast,
// is emitted regardless of runtime filtering: only the compile-time cap at
// the call site gates it.
//
// Records carry the file, line and function as attributes. Re-entrant calls
// (a record emitted from inside the export path, on the same task) still reach
// the console but emit no record.
//
// When CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED is off the whole call is
// discarded — esp_log_otel.h installs no wrappers in that build, so nothing
// routes through here and the stock ESP_LOGx macros write the console
// themselves.
void esp_opentelemetry_log_and_emit(esp_log_level_t level, const char* tag,
                                    const char* file, int line,
                                    const char* function, const char* format,
                                    ...) __attribute__((format(printf, 6, 7)));

#ifdef __cplusplus
}
#endif
