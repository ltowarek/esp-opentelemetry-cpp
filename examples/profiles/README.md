# profiles example

The statistical CPU profiler with the console profiles exporter: an OTLP/JSON `ProfilesData` document dumped to the serial console. No network and no debugger, so it runs under QEMU.

The exporter is constructed here in `main.cpp` and passed to the setup call —
the application chooses its exporter, as it does with the upstream SDK. Swap in
`esp_opentelemetry::MakeOtlpHttp*Exporter()` or a `Jtag*Exporter` and nothing
else about the example changes; [`../otlp/`](../otlp/) and
[`../jtag/`](../jtag/) do exactly that for every signal at once.

## Build

```sh
idf.py build
```

## Run under QEMU

```sh
bash test_qemu.sh
```

## Run on hardware

```sh
idf.py -p /dev/ttyACM0 flash monitor
```
