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
esp_opentelemetry_tracing_setup(CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME);  // traces
esp_opentelemetry_metrics_setup();                                       // metrics provider
esp_opentelemetry_logs_setup();                                          // logs provider
esp_opentelemetry_profiling_setup();                                     // statistical CPU profiler + span->profile link

// Create spans:
auto tracer = esp_opentelemetry_tracer();
auto span   = tracer->StartSpan("my.operation");
auto scope  = opentelemetry::trace::Scope(span);
span->End();

// Register metric instruments against the global meter provider:
auto meter = opentelemetry::metrics::Provider::GetMeterProvider()->GetMeter("my-device");
```

Ship a translation unit's existing `ESP_LOG` output by including one header —
the call sites keep their text and their console output:

```cpp
#include "esp_log.h"
#include "esp_log_otel.h"   // redefines ESP_LOGE/W/I; opt-in per translation unit

ESP_LOGI(TAG, "connected to %s", ssid);   // also emitted as a log record
```

Set the system time (SNTP) before emitting records — log records carry
absolute timestamps, and an ESP32 that has not synced reads as 1970, which
Loki and other backends reject.

The wrappers respect the project's existing compile-time cap
(`LOG_LOCAL_LEVEL` / `CONFIG_LOG_MAXIMUM_LEVEL`) rather than adding a second
filter, and evaluate their arguments exactly once, as the stock macros do. The
message is formatted once into a
`CONFIG_ESP_OPENTELEMETRY_LOGS_MAX_BODY_LEN` buffer and truncated there if it
does not fit — in the console line as well as in the record.

Enable signals via `idf.py menuconfig` → **OpenTelemetry** or set in `sdkconfig.defaults`:

```
CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME="my-device"
CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED=y
CONFIG_ESP_OPENTELEMETRY_TRACING_OTLP_BASE_URL="http://192.168.1.10:4318"
CONFIG_ESP_OPENTELEMETRY_METRICS_ENABLED=y
CONFIG_ESP_OPENTELEMETRY_METRICS_OTLP_BASE_URL="http://192.168.1.10:4318"
CONFIG_ESP_OPENTELEMETRY_LOGS_ENABLED=y
CONFIG_ESP_OPENTELEMETRY_LOGS_OTLP_BASE_URL="http://192.168.1.10:4318"
CONFIG_ESP_OPENTELEMETRY_PROFILING_ENABLED=y
CONFIG_ESP_OPENTELEMETRY_PROFILES_OTLP_BASE_URL="http://192.168.1.10:4319"   # the symbolizer
CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS=2     # per-task span slot for span profiles
```

When a signal's `..._ENABLED` option is off, the component still compiles and the API is available — all calls route to the SDK's built-in no-op provider (or return immediately) with zero runtime overhead.

The application constructs its exporter and passes it to the setup call, as it
does with the upstream SDK:

```cpp
esp_opentelemetry_tracing_setup(
    esp_opentelemetry::MakeOtlpHttpSpanExporter("http://192.168.1.10:4318"),
    {{"service.name", "my-device"}});
```

Kconfig decides only which exporter implementations are *compiled in*, since on
a 4 MB part you do not want to carry the ones you will not call:

| Exporter | Kconfig | Where signals go |
|----------|---------|------------------|
| OTLP/HTTP | `CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_HTTP` (default y) | `esp_opentelemetry::MakeOtlpHttp*Exporter()` — POSTed to a collector over Wi-Fi |
| Console | `CONFIG_ESP_OPENTELEMETRY_EXPORTER_OSTREAM` | The SDK's own ostream exporters; runs under QEMU |
| JTAG app-trace | `CONFIG_ESP_OPENTELEMETRY_EXPORTER_JTAG` | `esp_opentelemetry::Jtag*Exporter` — one OTLP/JSON document per line on the app-trace channel, relayed by a host-side forwarder; no network |

Each `esp_opentelemetry_*_setup()` also has a no-exporter overload that builds
an OTLP/HTTP exporter from that signal's `..._OTLP_BASE_URL`, which is what a
firmware that only ever exports over Wi-Fi wants.

Profiles have no exporter interface in opentelemetry-cpp, so the component
defines one (`esp_profiles_exporter.hpp`) and
`esp_opentelemetry_profiling_setup()` takes it the same way.

## Examples

| Example | Description | Hardware needed |
|---------|-------------|-----------------|
| [`examples/traces/`](examples/traces/) | One signal, simplest exporter: `OStreamSpanExporter` printing a parent/child span to the console. | None (QEMU) |
| [`examples/metrics/`](examples/metrics/) | One signal, simplest exporter: `OStreamMetricExporter` printing a counter. | None (QEMU) |
| [`examples/logs/`](examples/logs/) | One signal, simplest exporter: `OStreamLogRecordExporter` printing log records. | None (QEMU) |
| [`examples/profiles/`](examples/profiles/) | One signal, simplest exporter: the CPU profiler dumping OTLP/JSON `ProfilesData` to the console. | None (QEMU) |
| [`examples/otlp/`](examples/otlp/) | Every signal over OTLP/HTTP to a collector, with Wi-Fi and SNTP bring-up. | Wi-Fi |
| [`examples/jtag/`](examples/jtag/) | Every signal over one JTAG app-trace channel, forwarded to a collector by OpenOCD + Vector. | JTAG (no network) |
| [`examples/propagation/`](examples/propagation/) | W3C TraceContext inject across an HTTP boundary; logs the `traceparent` header injected into an outgoing request. | Wi-Fi |

The per-signal examples each show one signal with the simplest exporter there
is, so the signal's own API is the only thing on screen. The two transport
examples show every signal at once, because what varies there is the exporter —
they differ from each other only in which exporter each signal is handed.

## Exporter cost

Measured on the [dust-mite](https://github.com/ltowarek/dust-mite) car firmware
(ESP32-S3, VGA MJPEG stream over Wi-Fi), comparing the same firmware with no
telemetry, with the OTLP/HTTP exporters, and with the JTAG exporters. All four
signals are enabled in the two telemetry arms; the JTAG arm was measured with
OpenOCD attached and draining the channel, so its writes are not free.

| | no telemetry | OTLP/HTTP | JTAG |
|---|---:|---:|---:|
| Application binary | 1.02 MB | 2.57 MB | 2.40 MB |
| Stream frame rate | 16.65–16.77 fps | 16.78–16.82 fps | 16.47–16.72 fps |

Runtime cost is within measurement noise: the spread across arms
(16.47–16.82 fps) is inside the run-to-run spread of a single arm. Flash cost
is substantial: telemetry adds ~1.5 MB, most of it protobuf and the OTLP
exporter family. The
JTAG exporters are ~170 KB smaller than OTLP/HTTP, because they reuse the SDK's
OTLP *file* exporter and skip the HTTP client.

Frame rate here is bandwidth-bound, so it only compares across arms when the
mean JPEG frame size matches. Arms measured minutes apart under changing light
produced frame sizes from 15.7 to 32.0 KiB and apparent frame-rate differences
of 40%, all of it the camera rather than the exporter. The numbers above come
from flashing prebuilt binaries back to back, and every arm landed within
31.4–32.0 KiB.

Build time follows the same split: the `otlp` example takes roughly three times
as long as the `jtag` example from a cold build directory, the OTLP/HTTP
exporter family being the difference.

## Workarounds

The `src/workarounds/` subtree contains code that exists purely to paper over upstream deficiencies in third-party libraries or the Xtensa toolchain. Each workaround should be removable once the upstream issue is resolved, and each is tracked by an issue here so the reason survives the code.

| File | Root cause | Upstream | Issue |
|------|-----------|----------|-------|
| `src/workarounds/posix_shims.c` | `nanosleep` missing from newlib; `pthread_atfork` missing (causes libnosys collision); `sysconf(_SC_PAGESIZE)` returns -1 (causes Abseil `LowLevelAlloc` overflow); THREADPTR uninitialised before FreeRTOS scheduler (crashes `thread_local` during global ctors); `linkat` missing, referenced by the OTLP file exporter's rotation path | Abseil, ESP-IDF newlib, opentelemetry-cpp | [#17](https://github.com/ltowarek/esp-opentelemetry-cpp/issues/17), [#18](https://github.com/ltowarek/esp-opentelemetry-cpp/issues/18), [#21](https://github.com/ltowarek/esp-opentelemetry-cpp/issues/21), [#96](https://github.com/ltowarek/esp-opentelemetry-cpp/issues/96) |
| `src/workarounds/absl_varint_bool.h` | `int32_t` is `long` not `int` on Xtensa; `bool`/`int`/`pid_t` do not match any `EncodeVarint` overload — ambiguous call on GCC 13.2 | Abseil | [#23](https://github.com/ltowarek/esp-opentelemetry-cpp/issues/23) |
| `src/workarounds/sys/mman.h` | `sys/mman.h` absent from newlib; Abseil `LowLevelAlloc` calls `mmap` to grow its arena | Abseil | [#19](https://github.com/ltowarek/esp-opentelemetry-cpp/issues/19) |
| `src/workarounds/time.h` | `struct tm` in newlib lacks `tm_gmtoff`; Abseil cctz includes it unconditionally | Abseil cctz | [#20](https://github.com/ltowarek/esp-opentelemetry-cpp/issues/20) |
| `src/workarounds/absl_shadow/absl/base/internal/thread_identity.h` | Abseil's `thread_identity.h` has an unconditional `static_assert(std::atomic<WaitState>::is_always_lock_free)` (`WaitState` is `enum class : uint8_t`). The C++ standard does not require 1-byte atomics to be always-lock-free, so the assert fails on ESP toolchain configurations where it is not (reproduced in esp_otel CI). A header-shadow shim (the `absl_shadow` dir is prepended ahead of Abseil's `-I`) brackets only its `#include_next` of the upstream header with `push_macro`/`pop_macro`, rewriting the `is_always_lock_free` token so the failing assert becomes `... \|\| true` while the companion cache-line assert is left intact. | Abseil | [#59](https://github.com/ltowarek/esp-opentelemetry-cpp/issues/59) |
| `src/workarounds/pb_defaults.lf` | protobuf places `dummy_weak_default` in a `pb_defaults` section when `PROTOBUF_DESCRIPTOR_WEAK_MESSAGES_ALLOWED` is defined (`port_def.inc` guards it on `__clang__`) and reads the `__start_`/`__stop_` symbols a linker synthesises for an output section of that name. ESP-IDF places no such section and IDF v6 rejects orphan sections, so the executable fails to link. The fragment merges the section into `flash_rodata` and `CMakeLists.txt` defines the symbol pair as an empty range, leaving `InitWeakDefaults()` nothing to walk — sound only while nothing is generated with `--descriptor_implicit_weak_messages` | protobuf, ESP-IDF | [#94](https://github.com/ltowarek/esp-opentelemetry-cpp/issues/94) |
| `src/workarounds/esp_heap_align.cpp` | ESP-IDF heap uses `sizeof(void*)=4` as its alignment granularity; `alignof(std::max_align_t)==8` on Xtensa; `operator new` is therefore non-conforming. `google::protobuf::Arena` / `TaggedAllocationPolicyPtr` stores flags in the low 3 bits of a pointer (`kPtrMask=~7`), requiring 8-byte alignment. A 4-byte-aligned block causes `get()` to read 4 bytes before the struct, treating `max_block_size` (`0x00010000`) as a function pointer → `InstrFetchProhibited` at `PC=0x00010000`. Replaces the six standard replaceable allocation operators with `heap_caps_aligned_alloc`-backed versions. | ESP-IDF heap | [#32](https://github.com/ltowarek/esp-opentelemetry-cpp/issues/32) |

## ESP-specific integrations

The `src/integration/` subtree contains code that is deliberately ESP32-specific and is part of the component's defined scope.

| File | What it provides |
|------|-----------------|
| `src/integration/esp_http_client_transport.cpp` | `HttpClient` implementation backed by `esp_http_client`, passed directly to `OtlpHttpExporter`'s HTTP-client constructor overload ([open-telemetry/opentelemetry-cpp#4071](https://github.com/open-telemetry/opentelemetry-cpp/pull/4071)), replacing libcurl for the OTLP/HTTP exporter |
| `src/integration/esp_tracing.cpp` | `esp_opentelemetry_tracing_setup()` / `esp_opentelemetry_tracer()` — ESP-friendly wiring of exporter, processor (64 KB PSRAM export-thread stack), resource, and W3C propagator via Kconfig |
| `src/integration/esp_jtag_exporters.cpp` | `esp_opentelemetry::JtagSpanExporter` / `JtagLogRecordExporter` / `JtagMetricExporter` — OTLP/JSON written to the ESP-IDF app-trace (JTAG) channel instead of the network; reuses the SDK's OTLP file exporters through a custom `OtlpFileAppender`, so the encoding is identical to the OTLP/HTTP exporters' |
| `src/integration/esp_jtag_channel.cpp` | The single app-trace writer behind every JTAG exporter: chunks a document into the buffer, terminates a truncated line so the stream resynchronises, and serialises whole documents so concurrent signals cannot interleave. Profiles, whose exporter is hand-rolled, write through it directly |
| `src/integration/esp_metrics.cpp` | `esp_opentelemetry_metrics_setup()` — `PeriodicExportingMetricReader` + OTLP/HTTP metric exporter; `observe_double/observe_int64` helpers over the `ObserverResult` variant API |
| `src/integration/esp_logs.cpp` | `esp_opentelemetry_logs_setup()` / `esp_opentelemetry_logger()` — `BatchLogRecordProcessor` + OTLP/HTTP log record exporter; `esp_opentelemetry_log_and_emit()`, the bridge the `esp_log_otel.h` `ESP_LOGx` wrappers expand to |
| `include/esp_jtag_exporters.hpp` | Public declarations of the JTAG exporters, one class per signal, each compiled away when its signal or `CONFIG_ESP_OPENTELEMETRY_EXPORTER_JTAG` is off. Application code constructs one and passes it to the matching `..._setup()` call, as it would upstream |
| `include/esp_otlp_http_exporters.hpp` / `src/integration/esp_otlp_http_exporters.cpp` | `MakeOtlpHttp*Exporter()` — the SDK's OTLP/HTTP exporters bound to `esp_http_client`, since upstream's own factories build a libcurl client that does not cross-compile to Xtensa |
| `include/esp_profiles_exporter.hpp` | `ProfilesExporter` — the exporter interface opentelemetry-cpp has for every signal except profiles. Shaped like the SDK's, so profiles are selected the same way: `JtagProfilesExporter` sits with the other JTAG classes, `MakeOtlpHttpProfilesExporter()` with the other OTLP/HTTP factories, and `src/integration/esp_profiles_exporter.cpp` holds the console one |
| `include/esp_log_otel.h` | `ESP_LOGE`/`ESP_LOGW`/`ESP_LOGI` wrappers capturing the call site's file, line and function, gated on the project's own `ESP_LOG_ENABLED()` compile-time cap. Opt-in per translation unit; not pulled in by `esp_opentelemetry.hpp` |
| `src/integration/esp_profiling.cpp` | `esp_opentelemetry_profiling_setup()` — per-core gptimer-ISR statistical sampler (`esp_backtrace`), lock-free rings, stack aggregation |
| `src/integration/esp_profiles_document.cpp` | `esp_opentelemetry::export_profiles()` — builds the OTLP profiles (`v1development`) document with cJSON and hands it to the installed `ProfilesExporter`; opentelemetry-cpp has no profiles SDK to build it for us |
| `src/integration/esp_task_span_slot.cpp` | Per-task active-span slot (FreeRTOS TLS + seqlock) mirroring `Scope` activation — the FreeRTOS analog of Go's goroutine labels; `esp_opentelemetry_active_span_id()` ISR-safe reader; span stamping with the configurable `CONFIG_ESP_OPENTELEMETRY_PROFILES_SPAN_ATTRIBUTE` |
| `tools/symbolizer/` | Host-side OTLP profiles symbolizer: `xtensa-esp-elf-addr2line` resolution against build ELFs (auto-discovered by sha256 = profile `build_id`), ISR-frame trimming, forwards to an OTLP collector |

## Tested OTel C++ SDK features

Features validated on ESP32 hardware or QEMU. Untested features compile but have not been exercised end-to-end on device.

| Feature | Status | Example |
|---------|--------|---------|
| `OStreamSpanExporter` | Tested (QEMU) | [`examples/traces/`](examples/traces/) |
| `SimpleSpanProcessor` | Linked — `esp_opentelemetry_tracing_setup()` always installs a `BatchSpanProcessor` | — |
| `BatchSpanProcessor` | Tested (hardware, ESP32-S3) | [`examples/otlp/`](examples/otlp/) |
| `OtlpHttpExporter` (JSON) | Tested (hardware, ESP32-S3) | [`examples/otlp/`](examples/otlp/) |
| W3C TraceContext propagation (inject) | Tested (hardware) | [`examples/propagation/`](examples/propagation/) |
| `OtlpFileExporter` / `OtlpFileLogRecordExporter` / `OtlpFileMetricExporter` with a custom `OtlpFileAppender` (OTLP/JSON over JTAG app-trace) | Tested (hardware, ESP32-S3; OpenOCD 0.12 / Vector 0.50 / collector 0.156) — all four signals on one channel, verified through to Tempo/Loki | [`examples/jtag/`](examples/jtag/) |
| Span attributes (`SetAttribute`) | Tested | covered by all examples |
| `PeriodicExportingMetricReader` + `OtlpHttpMetricExporter` (JSON) | Tested (hardware, ESP32-S3) | [`examples/otlp/`](examples/otlp/) |
| OTLP profiles (`v1development`, JSON) + span profiles | Tested (hardware, ESP32-S3; Pyroscope 1.18 / collector 0.146) | [`examples/profiles/`](examples/profiles/) (QEMU), [`examples/otlp/`](examples/otlp/) |
| Custom `RuntimeContextStorage` (per-task span slot) | Tested (hardware, ESP32-S3 + QEMU) | [`examples/profiles/`](examples/profiles/) |
| Span events (`AddEvent`) | Untested | — |
| `OtlpHttpExporter` (protobuf) | Untested | — |
| `OStreamMetricExporter` | Tested (QEMU) | [`examples/metrics/`](examples/metrics/) |
| `PeriodicExportingMetricReader` | Tested (QEMU) | [`examples/metrics/`](examples/metrics/) |
| `OtlpHttpMetricExporter` | Linked | — |
| Counter instrument (`Add`) | Tested (QEMU) | [`examples/metrics/`](examples/metrics/) |
| Observable gauge (`AddCallback`) | Linked | — |
| `OStreamLogRecordExporter` | Tested (QEMU) | [`examples/logs/`](examples/logs/) |
| `SimpleLogRecordProcessor` | Linked — `esp_opentelemetry_logs_setup()` always installs a `BatchLogRecordProcessor` | — |
| `BatchLogRecordProcessor` | Tested (hardware, ESP32-S3) | [`examples/otlp/`](examples/otlp/) |
| `OtlpHttpLogRecordExporter` (JSON) | Tested (hardware, ESP32-S3; Loki 3.7 / collector 0.156) | [`examples/otlp/`](examples/otlp/) |
| Log record attributes + `ESP_LOG` call-site capture | Tested (hardware, ESP32-S3 + QEMU) | [`examples/logs/`](examples/logs/) (QEMU), [`examples/otlp/`](examples/otlp/) |
