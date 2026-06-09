# batch example

Demonstrates `BatchSpanProcessor` + `OtlpHttpExporter` (JSON). Connects to a
Wi-Fi network, creates a root span with five child spans, and exports them to
an OTLP collector via HTTP POST. Thread stacks are allocated on PSRAM to avoid
exhausting the 520 KB internal DRAM.

Wi-Fi and a reachable OTLP collector are required.

## What to observe

After flashing, the serial console logs span creation and the OTLP flush:

```
I batch-example: Connecting to Wi-Fi SSID: <ssid> ...
I batch-example: Wi-Fi connected
I batch-example: Queueing child spans...
I batch-example: Done. Check your OTLP collector for service 'batch-example'.
```

On the collector side you should see one trace with a root span `batch.demo`
and five child spans `batch.item.0` through `batch.item.4`.

## Configure

Set the following in `sdkconfig.defaults` or via `idf.py menuconfig`:

| Key | Description |
|-----|-------------|
| `CONFIG_WIFI_SSID` | Wi-Fi network name |
| `CONFIG_WIFI_PASSWORD` | Wi-Fi password |
| `CONFIG_ESP_OPENTELEMETRY_EXPORTER_OTLP_ENDPOINT` | OTLP collector endpoint, e.g. `http://192.168.1.10:4318` |

## Build

```sh
idf.py build
```

## Run on hardware

```sh
idf.py -p /dev/ttyUSB0 flash monitor
```
