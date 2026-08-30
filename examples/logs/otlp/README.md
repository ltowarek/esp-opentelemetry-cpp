# logs/otlp example

Demonstrates `esp_opentelemetry_logs_setup()`: a `BatchLogRecordProcessor`
feeding `OtlpHttpLogRecordExporter` (JSON) over `esp_http_client`. Connects to
a Wi-Fi network, then logs a line per second through ordinary `ESP_LOGI` calls
(with a warning every fifth tick and an error every tenth), all shipped by
including [`esp_log_otel.h`](../../../include/esp_log_otel.h).

Tracing and metrics stay disabled to show the logs signal standing alone.

The `vcs.*` resource attributes are stamped the same way the tracing and
metrics examples stamp them. `code.filepath` is whatever `__FILE__` expands to
in the consuming project — here, a path relative to this example — so a
backend resolving records back to GitHub source needs a repository and root
path that match those paths.

The example runs SNTP before installing the provider. Log records carry
absolute timestamps and the ESP32 has no clock of its own, so without this
every record is stamped 1970 and Loki rejects it with "timestamp too old".

Wi-Fi and a reachable OTLP logs receiver are required (an OpenTelemetry
Collector, or a store with native OTLP ingestion such as Loki at
`http://<host>:3100/otlp`).

## What to observe

```
I logs-otlp-example: Wi-Fi connected
I logs-otlp-example: System time set
I logs-otlp-example: OpenTelemetry logs enabled -> http://192.168.1.10:4318
I logs-otlp-example: Logs OTLP base URL: http://192.168.1.10:4318
I logs-otlp-example: tick 0, uptime 3s
```

On the receiver side the same lines appear under service `logs-otlp-example`,
at severity INFO/WARN/ERROR, each carrying `log.tag`, `code.filepath`,
`code.lineno` and `code.function`.

## Configure

| Key | Description |
|-----|-------------|
| `CONFIG_WIFI_SSID` / `CONFIG_WIFI_PASSWORD` | Wi-Fi credentials |
| `CONFIG_ESP_OPENTELEMETRY_LOGS_OTLP_BASE_URL` | OTLP logs receiver; `/v1/logs` is appended |
| `CONFIG_ESP_OPENTELEMETRY_LOGS_BATCH_MAX_QUEUE_SIZE` | Records queued before new ones are dropped |
| `CONFIG_ESP_OPENTELEMETRY_LOGS_MAX_BODY_LEN` | Formatted-message buffer; longer messages are truncated |

## Build

```sh
idf.py build
```

## Run on hardware

```sh
idf.py -p /dev/ttyUSB0 flash monitor
```
