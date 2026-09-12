#!/usr/bin/env bash
set -euo pipefail

timeout 120 idf.py qemu 2>&1 | tee /tmp/qemu.log || true

# The ESP log exporter delegates to OStreamSpanExporter, so one span becomes
# several ESP_LOGI lines (its own field-per-line block) rather than one; every
# line prints at INFO, including the Error-status span's.
grep -qE "otel\.span:\s+name\s+: work\.iteration" /tmp/qemu.log \
    || { echo "FAIL: no otel.span line with the span's name"; exit 1; }

grep -qE "otel\.span:.*service\.name: traces-example" /tmp/qemu.log \
    || { echo "FAIL: no otel.span line with the example's service.name"; exit 1; }

grep -qE "otel\.span:\s+status\s+: Error" /tmp/qemu.log \
    || { echo "FAIL: no otel.span line for the Error-status span"; exit 1; }

grep -qE "otel\.span:.*name\s+: work\.step\.checkpoint" /tmp/qemu.log \
    || { echo "FAIL: no otel.span line for the span event"; exit 1; }

echo "PASS"
