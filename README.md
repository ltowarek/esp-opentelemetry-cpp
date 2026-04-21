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

// Propagate context across protocol boundaries (e.g. cJSON payloads):
esp_opentelemetry_inject_traceparent(json_object);
auto ctx = esp_opentelemetry_extract_traceparent(json_object);
```

Enable tracing via `idf.py menuconfig` → **Tracing** or set in `sdkconfig.defaults`:

```
CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED=y
CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_ENDPOINT="http://192.168.1.10:4318"
CONFIG_ESP_OPENTELEMETRY_SERVICE_NAME="my-device"
```

When `CONFIG_ESP_OPENTELEMETRY_TRACING_ENABLED` is off, the component still compiles and the API is available — all calls route to the SDK's built-in no-op provider with zero runtime overhead.

## Examples

See [`examples/basic/`](examples/basic/) for a minimal ESP32-S3 application.
