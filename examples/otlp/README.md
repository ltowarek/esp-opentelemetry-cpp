# otlp example

Every signal over OTLP/HTTP. `main.cpp` constructs a `MakeOtlpHttp*Exporter()`
per signal and passes each to the matching `esp_opentelemetry_*_setup()` call.
Brings up Wi-Fi and sets the clock over SNTP first.

[`../jtag/`](../jtag/) is the same example with `Jtag*Exporter`s in place of
those four, and without the Wi-Fi and SNTP bring-up that exporting over the
network requires. The per-signal examples ([`../traces/`](../traces/),
[`../metrics/`](../metrics/), [`../logs/`](../logs/),
[`../profiles/`](../profiles/)) show one signal each with the console
exporters.

## What to observe

Once a second: a `work.iteration` span with a `work.step` child, an
`iterations` counter increment, and an "iteration complete" log record, all
carrying `"service.name": "otlp-example"`; the profiler exports a
`ProfilesData` document every 5 s. Each signal arrives at the endpoint
configured for it.

## Configure

Set the Wi-Fi credentials and one base URL per signal in
`sdkconfig.defaults`, or through `idf.py menuconfig`:

```
CONFIG_WIFI_SSID="..."
CONFIG_WIFI_PASSWORD="..."
CONFIG_ESP_OPENTELEMETRY_TRACING_OTLP_BASE_URL="http://192.168.1.10:4318"
CONFIG_ESP_OPENTELEMETRY_LOGS_OTLP_BASE_URL="http://192.168.1.10:4318"
CONFIG_ESP_OPENTELEMETRY_METRICS_OTLP_BASE_URL="http://192.168.1.10:8080/otlp"
CONFIG_ESP_OPENTELEMETRY_PROFILES_OTLP_BASE_URL="http://192.168.1.10:4319"
```

The three differ in practice: metrics often go to a Prometheus-compatible
store's own OTLP endpoint, and profiles go to the symbolizer
([`tools/symbolizer/`](../../tools/symbolizer/)), which resolves raw PCs
against the firmware ELF before forwarding. A signal whose URL is empty stays
disabled.

## Timestamps

`sync_time()` blocks on SNTP before any signal is emitted. Without it the
ESP32's clock reads 1970 plus uptime, and backends that reject old data — Loki,
for one — drop every record while the export itself reports success.

## Build and run

```sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```
