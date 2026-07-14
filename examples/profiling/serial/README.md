# profiling/serial example

Demonstrates the statistical CPU profiler standing alone — no Wi-Fi, no
backend. Profiles are dumped to the serial console as OTLP/JSON
(`CONFIG_ESP_OPENTELEMETRY_PROFILES_DEBUG_JSON`, `PROFILE_JSON_BEGIN`/`END`
markers), so the whole sampler → span slot → exporter path is observable on
any board without any infrastructure — useful as a first check that the
sampler works before wiring up Wi-Fi and a backend (see
[`examples/profiling/otlp/`](../otlp/)).

Since there is no Wi-Fi and no tracing backend here, there is no real tracer
to draw a span from — enabling one would pull in the SDK's network-touching
exporter without ever bringing up Wi-Fi (that's what the `otlp` variant is
for). The example builds a `SpanContext` with a randomly generated id instead
(`esp_fill_random()`, the same primitive the SDK's own id generator would
use) and wraps the hot loop in it with `trace::Scope`: any span, real or
otherwise, that is active this way while a sample is taken gets linked
through the per-task active-span slot exactly like a real SDK span would. The
linked samples carry a `span_id` attribute in the dumped JSON.

`test_qemu.sh` runs this example in QEMU and checks for a dumped profile with
a span-linked sample — QEMU's timing is not representative, but it proves the
sampler → span slot → exporter machinery runs end to end.

## What to observe

```
I esp_otel_profiling: profiling started: 100 Hz/core, depth 16, export every 1000 ms
PROFILE_JSON_BEGIN
{"resourceProfiles":[...],"dictionary":{...,"stackTable":[...]}}
PROFILE_JSON_END
```

The raw PCs in `CONFIG_ESP_OPENTELEMETRY_PROFILING_DEBUG_LOG` output (if
enabled) are symbolized automatically by `idf.py monitor` against the ELF.

## Build

```sh
idf.py build
```

## Run in QEMU

```sh
bash test_qemu.sh
```

## Run on hardware

```sh
idf.py -p /dev/ttyUSB0 flash monitor
```
