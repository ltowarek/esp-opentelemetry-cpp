#!/usr/bin/env bash
set -euo pipefail

timeout 120 idf.py qemu 2>&1 | tee /tmp/qemu.log || true

grep -qE "I \([0-9]+\) otel\.log: .*iteration complete" /tmp/qemu.log \
    || { echo "FAIL: no otel.log INFO line for the INFO record"; exit 1; }

# Anchors the tag-filtering absence check below: same bridge path, unfiltered
# tag, so its record must reach the exporter and print.
grep -qE "I \([0-9]+\) otel\.log: .*bridged and visible" /tmp/qemu.log \
    || { echo "FAIL: no otel.log INFO line for the unfiltered bridged record"; exit 1; }

if grep -qE "otel\.log: .*filtered by level" /tmp/qemu.log; then
    echo "FAIL: DEBUG record printed despite being below the default level"
    exit 1
fi

if grep -qE "otel\.log: .*filtered by tag" /tmp/qemu.log; then
    echo "FAIL: bridged record printed despite its original tag being filtered"
    exit 1
fi

echo "PASS"
