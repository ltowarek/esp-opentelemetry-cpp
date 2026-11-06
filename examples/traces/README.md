# traces example

The ESP log exporter + the tracing setup: a parent span with a child, a span event and an Error-status span, printed to the serial console under the `otel.span` tag. No network and no debugger, so it runs under QEMU.

The exporter is constructed in `main.cpp` and passed to the setup call, the
same way application code selects an exporter upstream.

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
