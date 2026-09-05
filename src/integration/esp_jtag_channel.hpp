// Serialised writer for the ESP-IDF app-trace (JTAG) channel.
//
// Every JTAG exporter — spans, log records, metrics, profiles — writes
// newline-delimited JSON into the one app-trace channel the target has, from
// whichever task produced the signal. Routing them through this writer keeps
// one document's chunks from interleaving with another's, which would leave
// the host with two unparseable lines instead of two records.

#pragma once

#include "sdkconfig.h"

#if defined(CONFIG_ESP_OPENTELEMETRY_EXPORTER_JTAG)

#include <chrono>
#include <cstddef>

namespace esp_opentelemetry {
namespace jtag_channel {

// Write one NDJSON record: data, then exactly one newline (a newline already
// at the end of data is not doubled). Returns false when the document was
// dropped, which the caller can log but not retry — app-trace has no
// backpressure signal beyond the write timeout.
bool WriteDocument(const char* data, std::size_t size);

// Flush the channel. The wait is capped at
// CONFIG_ESP_OPENTELEMETRY_JTAG_TIMEOUT_US, so an SDK default of
// "no timeout" cannot hang a shutdown with no host reading.
bool Flush(std::chrono::microseconds timeout);

}  // namespace jtag_channel
}  // namespace esp_opentelemetry

#endif  // CONFIG_ESP_OPENTELEMETRY_EXPORTER_JTAG
