# profiling/otlp example

Demonstrates the full profiles pipeline into a Grafana-compatible platform:
the statistical CPU profiler samples the interrupted call stack per core
(`esp_backtrace`, `CONFIG_ESP_OPENTELEMETRY_PROFILING_SAMPLE_HZ`, default
100 Hz/core; keep ≤ 200 — Xtensa unwinding is not free), aggregates identical
stacks into counts (pprof semantics), and exports OpenTelemetry **profiles**
(OTLP/HTTP JSON, `v1development`). Connects to Wi-Fi and loops a recognizable
hot function (`burn_cpu`) inside a real tracer span.

Wi-Fi, a running [`tools/symbolizer/`](../../../tools/symbolizer/), an
OpenTelemetry Collector with a `profiles` pipeline, and a profile backend
(e.g. Grafana Pyroscope) are required.

## Pipeline

The device sends **unsymbolized** profiles: raw addresses plus a `build_id`
equal to the ELF's sha256. The symbolizer — the device's
`CONFIG_ESP_OPENTELEMETRY_PROFILES_OTLP_BASE_URL` — resolves addresses with
`xtensa-esp-elf-addr2line` (ELFs auto-discovered by hash from mounted build
dirs), trims the timer-ISR dispatch frames, and forwards symbolized profiles
to the collector. See the symbolizer README for its configuration and why
symbolization is host-side.

## Span profiles

Every sample is labelled with the `span_id` active on the interrupted task
(mirrored into a per-task slot on `opentelemetry::trace::Scope` activation;
requires `CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS >= 2`). Spans are also
stamped with the `CONFIG_ESP_OPENTELEMETRY_PROFILES_SPAN_ATTRIBUTE` attribute
(default: the Grafana span-profiles convention `pyroscope.profile.id`; set
empty to disable) so a trace backend can link a span to the flame graph of CPU
sampled while exactly that span was active (Tempo's "Profiles for this span").

Expectations: at 100 Hz a 5 ms span catches at most one sample — per-span
flame graphs are meaningful for spans of tens of milliseconds or in aggregate
across many span instances.

## What to observe

```
I profiling-otlp: Wi-Fi connected
I esp_otel_profiling: profiling started: 100 Hz/core, depth 16, export every 2000 ms
I profiling-otlp: samples captured so far: 412
```

In the profile backend, the flame graph shows `burn_cpu()` under `app_main`
(split per FreeRTOS task via the `thread_name` label), and the `example.burn`
spans link to their samples via the span attribute.

## Configure

| Key | Description |
|-----|-------------|
| `CONFIG_WIFI_SSID` / `CONFIG_WIFI_PASSWORD` | Wi-Fi credentials |
| `CONFIG_ESP_OPENTELEMETRY_PROFILES_OTLP_BASE_URL` | The symbolizer, e.g. `http://192.168.1.10:4319` |
| `CONFIG_ESP_OPENTELEMETRY_TRACING_OTLP_BASE_URL` | OTLP collector for the spans, e.g. `http://192.168.1.10:4318` |

## Build

```sh
idf.py build
```

## Run on hardware

```sh
idf.py -p /dev/ttyUSB0 flash monitor
```
