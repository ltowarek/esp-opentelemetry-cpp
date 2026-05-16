# propagation example

Demonstrates W3C TraceContext inject across an HTTP boundary. The example
starts a span, injects the `traceparent` and `tracestate` headers into an
outgoing `esp_http_client` request, and logs the injected header values to the
serial console.

Wi-Fi and a reachable HTTP server are required.

## What to observe

After flashing, the serial console logs the injected header:

```
I propagation-example: inject: traceparent: 00-<trace-id>-<span-id>-01
```

On the server side, the `traceparent` header ties the incoming request to the
ESP32 trace, enabling distributed tracing across the HTTP boundary.

## Configure

Set the following in `sdkconfig.defaults` or via `idf.py menuconfig`:

| Key | Description |
|-----|-------------|
| `CONFIG_WIFI_SSID` | Wi-Fi network name |
| `CONFIG_WIFI_PASSWORD` | Wi-Fi password |
| `CONFIG_TARGET_URL` | HTTP URL the example sends to |
| `CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_ENDPOINT` | OTLP collector endpoint (optional; used to export spans) |

## Build

```sh
idf.py build
```

## Run on hardware

```sh
idf.py -p /dev/ttyUSB0 flash monitor
```
