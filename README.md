# esp-opentelemetry-cpp

ESP-IDF component integrating OpenTelemetry C++ SDK with ESP32 firmware

## Scope

This project is an **integration** of the upstream [opentelemetry-cpp](https://github.com/open-telemetry/opentelemetry-cpp) SDK with the [ESP-IDF](https://github.com/espressif/esp-idf) build system. It is not a fork and not a port — the vendored SDK submodule tracks a specific upstream commit and contains no local modifications. Where hardware constraints require deviations from upstream behaviour, the `src/workarounds/` subtree provides replacement translation units wired in through CMake `set_property(SOURCES)` overrides rather than edits to the submodule. The integration wires the SDK into the ESP-IDF build system and exposes a C++ API aligned with ESP-IDF naming conventions.

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

Enable tracing via `idf.py menuconfig` → **OpenTelemetry** or set in `sdkconfig.defaults`:

```
CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED=y
CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_ENDPOINT="http://192.168.1.10:4318"
CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME="my-device"
```

When `CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED` is off, the component still compiles and the API is available — all calls route to the SDK's built-in no-op provider with zero runtime overhead.

## Examples

| Example | Description | Hardware needed |
|---------|-------------|-----------------|
| [`examples/tracing/ostream/`](examples/tracing/ostream/) | `OStreamSpanExporter` + `SimpleSpanProcessor`; prints spans to the serial console. | None (QEMU) |
| [`examples/tracing/batch/`](examples/tracing/batch/) | `BatchSpanProcessor` + `OtlpHttpExporter`; Wi-Fi setup, PSRAM thread stack, exports spans to an OTLP collector. | Wi-Fi |
| [`examples/tracing/propagation/`](examples/tracing/propagation/) | W3C TraceContext inject across an HTTP boundary; logs the `traceparent` header injected into an outgoing request. | Wi-Fi |
| [`examples/metrics/ostream/`](examples/metrics/ostream/) | `OStreamMetricExporter` + `PeriodicExportingMetricReader`; counter and observable gauge printed to the serial console. | None (QEMU) |

## Workarounds

The `src/workarounds/` subtree contains code that exists purely to paper over upstream deficiencies in third-party libraries or the Xtensa toolchain. Each workaround should be removable once the upstream issue is resolved.

| File | Root cause | Upstream |
|------|-----------|----------|
| `src/workarounds/posix_shims.c` | `nanosleep` missing from newlib; `pthread_atfork` missing (causes libnosys collision); `sysconf(_SC_PAGESIZE)` returns -1 (causes Abseil `LowLevelAlloc` overflow); THREADPTR uninitialised before FreeRTOS scheduler (crashes `thread_local` during global ctors) | Abseil, ESP-IDF newlib |
| `src/workarounds/absl_varint_bool.h` | `int32_t` is `long` not `int` on Xtensa; `bool`/`int`/`pid_t` do not match any `EncodeVarint` overload — ambiguous call on GCC 13.2 | Abseil |
| `src/workarounds/sys/mman.h` | `sys/mman.h` absent from newlib; Abseil `LowLevelAlloc` calls `mmap` to grow its arena | Abseil |
| `src/workarounds/time.h` | `struct tm` in newlib lacks `tm_gmtoff`; Abseil cctz includes it unconditionally | Abseil cctz |
| `src/workarounds/absl_shadow/absl/base/internal/thread_identity.h` | Abseil's `thread_identity.h` has an unconditional `static_assert(std::atomic<WaitState>::is_always_lock_free)` (`WaitState` is `enum class : uint8_t`). The C++ standard does not require 1-byte atomics to be always-lock-free, so the assert fails on ESP toolchain configurations where it is not (reproduced in esp_otel CI). A header-shadow shim (the `absl_shadow` dir is prepended ahead of Abseil's `-I`) brackets only its `#include_next` of the upstream header with `push_macro`/`pop_macro`, rewriting the `is_always_lock_free` token so the failing assert becomes `... \|\| true` while the companion cache-line assert is left intact. | Abseil |
| `src/workarounds/esp_heap_align.cpp` | ESP-IDF heap uses `sizeof(void*)=4` as its alignment granularity; `alignof(std::max_align_t)==8` on Xtensa; `operator new` is therefore non-conforming. `google::protobuf::Arena` / `TaggedAllocationPolicyPtr` stores flags in the low 3 bits of a pointer (`kPtrMask=~7`), requiring 8-byte alignment. A 4-byte-aligned block causes `get()` to read 4 bytes before the struct, treating `max_block_size` (`0x00010000`) as a function pointer → `InstrFetchProhibited` at `PC=0x00010000`. Replaces the six standard replaceable allocation operators with `heap_caps_aligned_alloc`-backed versions. | ESP-IDF heap |

## ESP-specific integrations

The `src/integration/` subtree contains code that is deliberately ESP32-specific and is part of the component's defined scope.

| File | What it provides |
|------|-----------------|
| `src/integration/esp_http_client_transport.cpp` | `HttpClient` implementation backed by `esp_http_client`, passed directly to `OtlpHttpExporter`'s HTTP-client constructor overload ([open-telemetry/opentelemetry-cpp#4071](https://github.com/open-telemetry/opentelemetry-cpp/pull/4071)), replacing libcurl for the OTLP/HTTP exporter |
| `src/integration/esp_opentelemetry.cpp` | `esp_opentelemetry_setup()` / `esp_opentelemetry_tracer()` — ESP-friendly wiring of exporter, processor, resource, and W3C propagator via Kconfig |

## Tested OTel C++ SDK features

Features validated on ESP32 hardware or QEMU. Untested features compile but have not been exercised end-to-end on device.

| Feature | Status | Example |
|---------|--------|---------|
| `OStreamSpanExporter` | Tested (QEMU) | [`examples/tracing/ostream/`](examples/tracing/ostream/) |
| `SimpleSpanProcessor` | Tested (QEMU) | [`examples/tracing/ostream/`](examples/tracing/ostream/) |
| `BatchSpanProcessor` | Tested (hardware, ESP32-S3) | [`examples/tracing/batch/`](examples/tracing/batch/) |
| `OtlpHttpExporter` (JSON) | Tested (hardware, ESP32-S3) | [`examples/tracing/batch/`](examples/tracing/batch/) |
| W3C TraceContext propagation (inject) | Tested (hardware) | [`examples/tracing/propagation/`](examples/tracing/propagation/) |
| Span attributes (`SetAttribute`) | Tested | covered by all examples |
| Span events (`AddEvent`) | Untested | — |
| `OtlpHttpExporter` (protobuf) | Untested | — |
| `OStreamMetricExporter` | Tested (QEMU) | [`examples/metrics/ostream/`](examples/metrics/ostream/) |
| `PeriodicExportingMetricReader` | Tested (QEMU) | [`examples/metrics/ostream/`](examples/metrics/ostream/) |
| `OtlpHttpMetricExporter` | Linked | — |
| Counter instrument (`Add`) | Tested (QEMU) | [`examples/metrics/ostream/`](examples/metrics/ostream/) |
| Observable gauge (`AddCallback`) | Tested (QEMU) | [`examples/metrics/ostream/`](examples/metrics/ostream/) |
| Logs API | Not integrated | — |
