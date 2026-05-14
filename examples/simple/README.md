# simple example

> **Note:** This example is kept for CI compatibility. The canonical version is
> [`examples/ostream/`](../ostream/), which is identical in functionality but
> named after the feature it demonstrates.

Demonstrates `OStreamSpanExporter` + `SimpleSpanProcessor`. Spans are printed
to the serial console (stdout). No Wi-Fi required. Runs under QEMU.

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
idf.py -p /dev/ttyUSB0 flash monitor
```
