# esp-opentelemetry-cpp

ESP-IDF component integrating OpenTelemetry C++ SDK with ESP32 firmware

## Scope

This project is an **integration** of the upstream [opentelemetry-cpp](https://github.com/open-telemetry/opentelemetry-cpp) SDK with the [ESP-IDF](https://github.com/espressif/esp-idf) build system. It is not a fork and not a port — the vendored SDK is upstream, unmodified. The integration wires the SDK into the ESP-IDF build system and exposes a C++ API aligned with ESP-IDF naming conventions.

## Usage

Add this repository as a component under your project's `components/` directory (e.g. as a git submodule), then declare it as a dependency:

```cmake
idf_component_register(SRCS "main.cpp"
                        REQUIRES esp-opentelemetry-cpp)
```

```cpp
#include "esp_opentelemetry.hpp"

// Once, after Wi-Fi is up:
esp_opentelemetry_setup(CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME);

// Create spans:
auto tracer = esp_opentelemetry_tracer();
auto span   = tracer->StartSpan("my.operation");
auto scope  = opentelemetry::trace::Scope(span);
span->End();
```

Enable tracing via `idf.py menuconfig` → **Tracing** or set in `sdkconfig.defaults`:

```
CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED=y
CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_ENDPOINT="http://192.168.1.10:4318"
CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME="my-device"
```

When `CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED` is off, the component still compiles and the API is available — all calls route to the SDK's built-in no-op provider with zero runtime overhead.

## Examples

| Example | Description | Hardware needed |
|---------|-------------|-----------------|
| [`examples/ostream/`](examples/ostream/) | `OStreamSpanExporter` + `SimpleSpanProcessor`; prints spans to the serial console. | None (QEMU) |
| [`examples/batch/`](examples/batch/) | `BatchSpanProcessor` + `OtlpHttpExporter`; Wi-Fi setup, PSRAM thread stack, exports spans to an OTLP collector. | Wi-Fi |
| [`examples/propagation/`](examples/propagation/) | W3C TraceContext inject across an HTTP boundary; logs the `traceparent` header injected into an outgoing request. | Wi-Fi |

## Workarounds

The `src/workarounds/` subtree contains code that exists purely to paper over upstream deficiencies in third-party libraries or the Xtensa toolchain. Each workaround should be removable once the upstream issue is resolved.

| File | Root cause | Upstream |
|------|-----------|----------|
| `src/workarounds/posix_shims.c` | `nanosleep` missing from newlib; `pthread_atfork` missing (causes libnosys collision); `sysconf(_SC_PAGESIZE)` returns -1 (causes Abseil `LowLevelAlloc` overflow); THREADPTR uninitialised before FreeRTOS scheduler (crashes `thread_local` during global ctors) | Abseil, ESP-IDF newlib |
| `src/workarounds/absl_varint_bool.h` | `int32_t` is `long` not `int` on Xtensa; `bool`/`int`/`pid_t` do not match any `EncodeVarint` overload — ambiguous call on GCC 13.2 | Abseil |
| `src/workarounds/sys/mman.h` | `sys/mman.h` absent from newlib; Abseil `LowLevelAlloc` calls `mmap` to grow its arena | Abseil |
| `src/workarounds/time.h` | `struct tm` in newlib lacks `tm_gmtoff`; Abseil cctz includes it unconditionally | Abseil cctz |

## ESP-specific integrations

The `src/integration/` subtree contains code that is deliberately ESP32-specific and is part of the component's defined scope.

| File | What it provides |
|------|-----------------|
| `src/integration/esp_http_client_transport.cpp` | `HttpClientFactory` backed by `esp_http_client`, replacing libcurl for the OTLP/HTTP exporter |
| `src/integration/esp_opentelemetry.cpp` | `esp_opentelemetry_setup()` / `esp_opentelemetry_tracer()` — ESP-friendly wiring of exporter, processor, resource, and W3C propagator via Kconfig |

## Tested OTel C++ SDK features

Features validated on ESP32 hardware or QEMU. Untested features compile but have not been exercised end-to-end on device.

| Feature | Status | Example |
|---------|--------|---------|
| `OStreamSpanExporter` | Tested (QEMU) | [`examples/ostream/`](examples/ostream/) |
| `SimpleSpanProcessor` | Tested (QEMU) | [`examples/ostream/`](examples/ostream/) |
| `BatchSpanProcessor` | Tested (hardware, ESP32-S3) | [`examples/batch/`](examples/batch/) |
| `OtlpHttpExporter` (JSON) | Tested (hardware, ESP32-S3) | [`examples/batch/`](examples/batch/) |
| W3C TraceContext propagation (inject) | Tested (hardware) | [`examples/propagation/`](examples/propagation/) |
| Span attributes (`SetAttribute`) | Tested | covered by all examples |
| Span events (`AddEvent`) | Untested | — |
| `OtlpHttpExporter` (protobuf) | Untested | — |
| Metrics API | Not integrated | — |
| Logs API | Not integrated | — |
