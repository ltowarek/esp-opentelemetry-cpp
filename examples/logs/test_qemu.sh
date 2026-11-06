#!/usr/bin/env bash
set -euo pipefail

timeout 120 idf.py qemu 2>&1 | tee /tmp/qemu.log || true
grep -qE "I \([0-9]+\) otel\.log: .*iteration complete" /tmp/qemu.log && echo "PASS" || { echo "FAIL"; exit 1; }
