# metrics/otlp example

Demonstrates `esp_opentelemetry_metrics_setup()`: a
`PeriodicExportingMetricReader` feeding `OtlpHttpMetricExporter` (JSON) over
`esp_http_client`. Connects to a Wi-Fi network, registers an observable gauge
(`example.uptime`, via the `observe_int64` helper) and a counter
(`example.ticks`), and exports them every
`CONFIG_ESP_OPENTELEMETRY_METRICS_EXPORT_INTERVAL_MS` (default 500 ms).

Tracing stays disabled to show the metrics signal standing alone.

Wi-Fi and a reachable OTLP metrics receiver are required (an OpenTelemetry
Collector, or a store with native OTLP ingestion such as VictoriaMetrics at
`http://<host>:8428/opentelemetry`).

## What to observe

```
I metrics-otlp-example: Wi-Fi connected
I metrics-otlp-example: Metrics OTLP base URL: http://192.168.1.10:8428/opentelemetry
```

On the receiver side, `example.uptime` climbs by one per second and
`example.ticks` counts loop iterations, both under service
`metrics-otlp-example`.

## Configure

| Key | Description |
|-----|-------------|
| `CONFIG_WIFI_SSID` / `CONFIG_WIFI_PASSWORD` | Wi-Fi credentials |
| `CONFIG_ESP_OPENTELEMETRY_METRICS_OTLP_BASE_URL` | OTLP metrics receiver; `/v1/metrics` is appended |

## Build

```sh
idf.py build
```

## Run on hardware

```sh
idf.py -p /dev/ttyUSB0 flash monitor
```
