#!/usr/bin/env bash
set -euo pipefail

# Smoke test: the example dumps OTLP/JSON profiles to the console
# (CONFIG_ESP_OPENTELEMETRY_PROFILES_DEBUG_JSON). export_profiles() only
# dumps once it has aggregated at least one sample, so PROFILE_JSON_BEGIN
# appearing proves the sampler ran. The span id is random per boot (see
# main.cpp), so rather than match a specific value this checks for the
# "span_id" attribute key itself, present only on samples that were linked to
# the active span. QEMU timing is not representative — the machinery is what's
# under test.
timeout 90 idf.py qemu 2>&1 | tee /tmp/qemu.log || true
grep -q "PROFILE_JSON_BEGIN" /tmp/qemu.log || { echo "FAIL: no OTLP profile dumped"; exit 1; }
grep -q '"span_id"' /tmp/qemu.log || { echo "FAIL: no span-linked sample in profile"; exit 1; }
echo "PASS"
