#!/usr/bin/env bash
set -euo pipefail

timeout 120 idf.py qemu 2>&1 | tee /tmp/qemu.log || true
grep -qE "iterations" /tmp/qemu.log && echo "PASS" || { echo "FAIL"; exit 1; }
