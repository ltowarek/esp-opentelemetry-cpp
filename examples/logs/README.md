# logs example

The ESP log exporter + the logs setup: log records printed to the serial console under the `otel.log` tag, alongside a DEBUG record filtered out by level and a bridged `ESP_LOGx` line filtered out by its original tag's runtime level. No network and no debugger, so it runs under QEMU.

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
