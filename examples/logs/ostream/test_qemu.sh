#!/usr/bin/env bash
set -euo pipefail

# The example prints every log record through OStreamLogRecordExporter, so the
# console carries both the ESP-IDF log line and the exported record. Each check
# below is on the exported record (body/severity/attributes), not the log line.
timeout 60 idf.py qemu 2>&1 | tee /tmp/qemu_logs.log || true

fail() { echo "FAIL: $1"; exit 1; }
present() { grep -qE "$1" /tmp/qemu_logs.log; }

# Severity mapping: ESP_LOGI/W/E -> INFO/WARN/ERROR.
present "severity_text +: INFO"  || fail "no INFO record"
present "severity_text +: WARN"  || fail "no WARN record"
present "severity_text +: ERROR" || fail "no ERROR record"

# Body is the formatted message, not the format string.
present "body +: info from app_main" || fail "body not printf-formatted"

# Call-site capture: tag, file, line and function of the ESP_LOG call.
present "log\.tag: logs_example"     || fail "no log.tag attribute"
present "code\.filepath: .*main\.cpp" || fail "no code.filepath attribute"
present "code\.function: app_main"   || fail "no code.function attribute"
present "code\.lineno: [0-9]+"       || fail "no code.lineno attribute"

# Compile-time cap. ESP_LOGD is not wrapped and this build compiles DEBUG out
# entirely; capped.cpp's ESP_LOG_WARN LOG_LOCAL_LEVEL suppresses its INFO call
# site while leaving its WARN one exported.
! present "debug from app_main" \
    || fail "debug line survived the compile-time cap"
! present "info below this module's cap" \
    || fail "info line survived the module's LOG_LOCAL_LEVEL cap"
present "body +: warn above this module's cap" \
    || fail "no record from the capped module's WARN call site"

echo "PASS"
