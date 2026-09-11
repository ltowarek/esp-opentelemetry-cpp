#!/usr/bin/env bash
set -euo pipefail

timeout 120 idf.py qemu 2>&1 | tee /tmp/qemu.log || true

grep -qE "I \([0-9]+\) otel\.span: .*traces-example" /tmp/qemu.log \
    || { echo "FAIL: no otel.span INFO line with service.name"; exit 1; }

grep -qE "W \([0-9]+\) otel\.span: .*status: Error" /tmp/qemu.log \
    || { echo "FAIL: no otel.span WARN line for the Error-status span"; exit 1; }

span_line=$(grep -E "otel\.span: name: work\.step," /tmp/qemu.log | head -1)
[ -n "$span_line" ] || { echo "FAIL: no work.step span line"; exit 1; }
span_id=$(echo "$span_line" | grep -oE "span_id: [0-9a-f]{16}" | head -1 | cut -d' ' -f2)
[ -n "$span_id" ] || { echo "FAIL: could not extract work.step span_id"; exit 1; }

grep -qE "otel\.span: span_id: ${span_id}, name: work\.step\.checkpoint" /tmp/qemu.log \
    || { echo "FAIL: no event line repeating the span's span_id"; exit 1; }

echo "PASS"
