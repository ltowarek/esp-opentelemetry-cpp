#!/usr/bin/env bash
set -euo pipefail

timeout 60 idf.py qemu 2>&1 | tee /tmp/qemu.log || true
grep -q "service.name.*ostream-example" /tmp/qemu.log && echo "PASS" || { echo "FAIL"; exit 1; }
