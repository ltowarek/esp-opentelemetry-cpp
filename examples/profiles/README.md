# profiles example

The statistical CPU profiler with the ESP log profiles exporter: an OTLP/JSON `ProfilesData` document dumped to the serial console under the `otel.profile` tag, with no begin/end markers. No network and no debugger, so it runs under QEMU.

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
