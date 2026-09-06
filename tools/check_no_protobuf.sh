#!/usr/bin/env bash
# Fails if an ELF contains any protobuf or Abseil symbol.
#
# The claim "this firmware links neither protobuf nor Abseil" is only worth
# making if something checks it: both libraries are reached indirectly, through
# whichever SDK exporter target a signal links, so a single wrong target name
# puts megabytes back into the image without any other visible sign.
#
# Usage: check_no_protobuf.sh <elf> [<elf>...]
#
# Run it from an environment where the Xtensa toolchain is on PATH (ESP-IDF's
# export.sh); NM overrides the tool for another target.

set -euo pipefail

NM="${NM:-xtensa-esp32s3-elf-nm}"

if [ "$#" -lt 1 ]; then
    echo "usage: $(basename "$0") <elf> [<elf>...]" >&2
    exit 2
fi

status=0
for elf in "$@"; do
    if [ ! -f "$elf" ]; then
        echo "$(basename "$0"): no such file: $elf" >&2
        exit 2
    fi

    # Mangled names rather than demangled ones: nm -C would also match a
    # demangled string that merely mentions the namespace, and the mangled
    # prefixes are unambiguous.
    found=$("$NM" "$elf" | grep -E '_ZN6google8protobuf|_ZN4absl' || true)
    if [ -n "$found" ]; then
        count=$(printf '%s\n' "$found" | wc -l)
        echo "FAIL: $elf contains $count protobuf/Abseil symbols, e.g."
        # `|| true`: head closing the pipe early would otherwise make the
        # pipeline exit 141 under pipefail and abort before the next file.
        { printf '%s\n' "$found" | head -5; } || true
        status=1
    else
        echo "OK: $elf contains no protobuf or Abseil symbols"
    fi
done

exit "$status"
