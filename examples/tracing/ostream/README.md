# ostream example

Demonstrates `OStreamSpanExporter` + `SimpleSpanProcessor`. Spans are printed
to the serial console (stdout). No Wi-Fi required. Runs under QEMU.

## What to observe

After flashing (or running under QEMU), the serial console prints a JSON-like
span record containing `"service.name": "ostream-example"` and
`"example.type": "ostream"`.

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
