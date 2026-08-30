# logs/ostream example

Demonstrates the Logs API and the `ESP_LOGx` wrapper standing alone: an
`OStreamLogRecordExporter` behind a `SimpleLogRecordProcessor` prints every
record to the serial console, so no network is needed.

`main.cpp` and `capped.cpp` log through ordinary `ESP_LOGI`/`ESP_LOGW`/
`ESP_LOGE` calls. Including [`esp_log_otel.h`](../../../include/esp_log_otel.h)
is the only change that ships them: the serial output is unchanged, and each
call additionally emits a record carrying the formatted message plus the call
site's tag, file, line and function.

`capped.cpp` sets `LOG_LOCAL_LEVEL` to `ESP_LOG_WARN` for itself, showing that
the wrappers inherit the project's existing compile-time cap rather than
applying one of their own — its `ESP_LOGI` call site reaches neither the
console nor the exporter.

## What to observe

```
I (315) logs_example: info from app_main
{
  ...
  severity_text      : INFO
  body               : info from app_main
  ...
  attributes         :
    log.tag: logs_example
    code.filepath: .../main/main.cpp
    code.lineno: 36
    code.function: app_main
  ...
}
```

## Run

```sh
idf.py build
idf.py qemu          # or: idf.py -p /dev/ttyUSB0 flash monitor
```

[`test_qemu.sh`](test_qemu.sh) runs the same thing under QEMU and asserts the
severity mapping, the formatted body, the call-site attributes, and that the
capped call sites produce no records.
