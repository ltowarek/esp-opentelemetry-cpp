#!/usr/bin/env bash
set -euo pipefail

timeout 60 idf.py qemu 2>&1 | tee /tmp/qemu_metrics.log || true
grep -q "example.requests" /tmp/qemu_metrics.log && echo "PASS" || { echo "FAIL"; exit 1; }
