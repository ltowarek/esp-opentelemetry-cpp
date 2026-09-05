#include "esp_jtag_channel.hpp"

#if defined(CONFIG_ESP_OPENTELEMETRY_EXPORTER_JTAG)

#include "esp_app_trace.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <algorithm>
#include <cstdint>

namespace esp_opentelemetry {
namespace jtag_channel {

namespace {

constexpr const char* TAG = "esp_otel_jtag";

constexpr uint32_t kWriteTimeoutUs =
    CONFIG_ESP_OPENTELEMETRY_JTAG_TIMEOUT_US;
constexpr std::size_t kChunkSize =
    CONFIG_ESP_OPENTELEMETRY_JTAG_CHUNK_SIZE;

// Guards a whole document rather than a single write, so two signals exporting
// concurrently produce two clean lines instead of interleaved fragments.
SemaphoreHandle_t DocumentLock() {
  static SemaphoreHandle_t lock = xSemaphoreCreateMutex();
  return lock;
}

// Chunked because a document is larger than the app-trace buffer; each chunk
// has to fit in whatever space the current buffer has left.
bool WriteChunked(const char* data, std::size_t size, std::size_t* written) {
  std::size_t offset = 0;
  while (offset < size) {
    const std::size_t chunk = std::min(kChunkSize, size - offset);
    const esp_err_t err = esp_apptrace_write(data + offset, chunk, kWriteTimeoutUs);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "app-trace write failed (%s) after %u of %u bytes",
               esp_err_to_name(err), static_cast<unsigned>(offset),
               static_cast<unsigned>(size));
      *written = offset;
      return false;
    }
    offset += chunk;
  }
  *written = offset;
  return true;
}

uint32_t TimeoutToUs(std::chrono::microseconds timeout) {
  const std::chrono::microseconds cap(kWriteTimeoutUs);
  if (timeout < std::chrono::microseconds::zero() || timeout > cap) {
    return kWriteTimeoutUs;
  }
  return static_cast<uint32_t>(timeout.count());
}

}  // namespace

bool WriteDocument(const char* data, std::size_t size) {
  while (size > 0 && data[size - 1] == '\n') {
    --size;
  }
  if (size == 0) {
    return true;
  }

  SemaphoreHandle_t lock = DocumentLock();
  if (lock == nullptr || xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) {
    return false;
  }

  std::size_t written = 0;
  const bool ok = WriteChunked(data, size, &written);

  // A document cut short would run into the next one and cost the host both
  // records, so the line is terminated either way. Zero timeout: the write
  // that just failed did so because the buffer was full, and waiting it out
  // again would double what this export costs the caller.
  static const char kNewline = '\n';
  if (ok || written > 0) {
    (void)esp_apptrace_write(&kNewline, 1, ok ? kWriteTimeoutUs : 0);
  }

  if (ok) {
    // Without a flush the tail of the document sits in the buffer until the
    // next write fills it, which at a slow signal rate means the host sees
    // nothing for an unbounded time.
    const esp_err_t err = esp_apptrace_flush(kWriteTimeoutUs);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "app-trace flush failed (%s)", esp_err_to_name(err));
    }
  }

  xSemaphoreGive(lock);
  return ok;
}

bool Flush(std::chrono::microseconds timeout) {
  return esp_apptrace_flush(TimeoutToUs(timeout)) == ESP_OK;
}

}  // namespace jtag_channel
}  // namespace esp_opentelemetry

#endif  // CONFIG_ESP_OPENTELEMETRY_EXPORTER_JTAG
