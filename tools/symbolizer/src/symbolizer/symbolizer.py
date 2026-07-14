"""Address -> function/file/line resolution for OTLP profiles.

Profiles arrive from the device with a top-level profiles dictionary whose
Location entries carry raw Xtensa addresses and one Mapping. The firmware
build_id (app_elf_sha256) is carried as a dictionary attribute
(process.executable.build_id) and equals the sha256 of the ELF file, so the
matching ELF is found automatically: the firmware build directories are
mounted into this container and scanned on a lookup miss (hashes cached by
mtime/size). ``<build_id>.elf`` files placed in the symbols directory act as a
manual override, e.g. for archived builds. Addresses are resolved with
``xtensa-esp-elf-addr2line``, populating the dictionary's function_table /
string_table and each Location's line list so Pyroscope renders named frames.

We also drop the leading sampler/ISR-dispatch frames (the timer ISR plumbing
the unwinder captures before reaching the interrupted task) from every stack, by
function name — robust to compiler inlining, unlike an on-device frame count.
"""

from __future__ import annotations

import glob
import hashlib
import logging
import os
import re
import shutil
import subprocess

log = logging.getLogger("symbolizer.symbolizer")

ADDR2LINE = os.environ.get("SYMBOLIZER_ADDR2LINE", "xtensa-esp-elf-addr2line")
BUILD_ID_KEY = "process.executable.build_id"
# Glob patterns (colon-separated) for firmware ELFs, relative to the mounted
# firmware tree; covers the main app and the component test apps.
ELF_GLOBS = os.environ.get(
    "SYMBOLIZER_ELF_GLOBS",
    "/firmware/build/*.elf:/firmware/components/*/test_apps/build/*.elf",
)

# Leaf frames belonging to the sampler / timer-ISR dispatch, trimmed from every
# stack so the interrupted application code becomes the flame-graph leaf. The
# default covers this component's sampler (esp_profiling.cpp) and the
# gptimer/Xtensa interrupt-dispatch functions; override via
# SYMBOLIZER_TRIM_REGEX for apps with custom samplers. Kept specific so it
# cannot match unrelated application code.
ISR_FRAME_RE = re.compile(
    os.environ.get(
        "SYMBOLIZER_TRIM_REGEX",
        r"\(anonymous namespace\)::(on_timer|sample_stack)\b"
        r"|^gptimer_"
        r"|^_xt_|^_frxt_|^xt_highint"
        r"|^esp_backtrace",
    )
)


class Symbolizer:
    def __init__(self, symbols_dir: str, elf_globs: str = ELF_GLOBS) -> None:
        self._symbols_dir = symbols_dir
        self._elf_globs = [g for g in elf_globs.split(":") if g]
        # (path -> (mtime, size, sha256)) so rebuilt ELFs are re-hashed.
        self._hash_cache: dict[str, tuple[float, int, str]] = {}
        # (elf_path, address) -> (function, file, line); addresses repeat
        # almost entirely between export windows, so addr2line is only spawned
        # for never-seen addresses.
        self._addr_cache: dict[tuple[str, int], tuple[str, str, int]] = {}
        self._addr2line = shutil.which(ADDR2LINE)
        if self._addr2line is None:
            log.warning("%s not found; profiles forwarded unsymbolized", ADDR2LINE)

    def _sha256(self, path: str) -> str | None:
        try:
            st = os.stat(path)
            cached = self._hash_cache.get(path)
            if cached and cached[0] == st.st_mtime and cached[1] == st.st_size:
                return cached[2]
            with open(path, "rb") as f:
                digest = hashlib.file_digest(f, "sha256").hexdigest()
            self._hash_cache[path] = (st.st_mtime, st.st_size, digest)
            return digest
        except OSError:
            return None

    def elf_for_build_id(self, build_id: str | None) -> str | None:
        if not build_id:
            return None
        # Called once per export window (every few seconds); _find_elf()'s
        # glob is cheap and _sha256() is already memoized by _hash_cache, so
        # no separate build_id -> path cache is needed on top of that.
        return self._find_elf(build_id)

    def _find_elf(self, build_id: str) -> str | None:
        # Manual override first: an archived <build_id>.elf in the symbols dir.
        override = os.path.join(self._symbols_dir, f"{build_id}.elf")
        if os.path.isfile(override):
            return override
        # Auto-discovery: the build_id is the ELF's sha256, so scan the mounted
        # firmware build dirs and match by hash.
        for pattern in self._elf_globs:
            for path in glob.glob(pattern):
                if self._sha256(path) == build_id:
                    return path
        return None

    @staticmethod
    def _build_id(dictionary: dict) -> str | None:
        strings = dictionary.get("stringTable", [])
        for attr in dictionary.get("attributeTable", []):
            k = attr.get("keyStrindex")
            if k is not None and k < len(strings) and strings[k] == BUILD_ID_KEY:
                return attr.get("value", {}).get("stringValue")
        return None

    def _resolve(self, elf: str, addresses: list[int]) -> dict[int, tuple[str, str, int]]:
        """addr -> (function, file, line); innermost frame only (no -i).

        Memoized per (elf, address): only never-seen addresses reach
        addr2line, and windows with no new addresses spawn no subprocess.
        """
        result: dict[int, tuple[str, str, int]] = {}
        misses: list[int] = []
        for addr in addresses:
            hit = self._addr_cache.get((elf, addr))
            if hit is not None:
                result[addr] = hit
            else:
                misses.append(addr)
        if misses:
            fresh = self._resolve_uncached(elf, misses)
            for addr, res in fresh.items():
                self._addr_cache[(elf, addr)] = res
            result.update(fresh)
        return result

    def _resolve_uncached(self, elf: str, addresses: list[int]) -> dict[int, tuple[str, str, int]]:
        if not addresses:
            return {}
        args = [self._addr2line, "-e", elf, "-f", "-C"] + [f"0x{a:x}" for a in addresses]
        proc = subprocess.run(args, capture_output=True, text=True, check=False)
        if proc.returncode != 0:
            # A real toolchain failure (wrong version, corrupted/truncated ELF)
            # looks identical to "no debug info" downstream (empty stdout) unless
            # logged here — every address would otherwise silently resolve to "".
            log.warning("addr2line failed (rc=%d) for %s: %s", proc.returncode, elf,
                        proc.stderr.strip())
        out = proc.stdout.splitlines()
        result: dict[int, tuple[str, str, int]] = {}
        for i, addr in enumerate(addresses):
            func = out[2 * i].strip() if 2 * i < len(out) else "??"
            loc = out[2 * i + 1].strip() if 2 * i + 1 < len(out) else "??:0"
            file, _, line = loc.partition(":")
            try:
                line_no = int(re.match(r"\d+", line).group())  # strip " (discriminator N)"
            except (AttributeError, ValueError):
                line_no = 0
            result[addr] = (func if func != "??" else "", file, line_no)
        return result

    def symbolize(self, profiles: dict) -> dict:
        d = profiles.get("dictionary")
        if not d:
            return profiles
        elf = self.elf_for_build_id(self._build_id(d))
        if not elf or self._addr2line is None:
            log.info("no ELF/addr2line for build; forwarding unsymbolized")
            return profiles

        locations = d.get("locationTable", [])
        addresses = [int(loc["address"]) for loc in locations if "address" in loc]
        resolved = self._resolve(elf, addresses)

        strings: list[str] = d.setdefault("stringTable", [""])
        string_index: dict[str, int] = {s: i for i, s in enumerate(strings)}

        def intern(s: str) -> int:
            if s not in string_index:
                string_index[s] = len(strings)
                strings.append(s)
            return string_index[s]

        functions = d.setdefault("functionTable", [])
        func_index: dict[tuple[str, str], int] = {}

        def function_for(name: str, file: str) -> int:
            # Keyed by (name, file): two translation units can define a
            # same-named static/anonymous-namespace function, and addr2line
            # correctly resolves each occurrence to its own file.
            key = (name, file)
            if key not in func_index:
                func_index[key] = len(functions)
                functions.append({
                    "nameStrindex": intern(name),
                    "filenameStrindex": intern(file),
                })
            return func_index[key]

        # Per-location: fill the line list and record whether it is an ISR frame.
        is_isr = [False] * len(locations)
        for i, loc in enumerate(locations):
            if "address" not in loc:
                continue
            func, file, line = resolved.get(int(loc["address"]), ("", "", 0))
            if not func:
                continue
            is_isr[i] = bool(ISR_FRAME_RE.search(func))
            loc["lines"] = [{"functionIndex": function_for(func, file), "line": str(line)}]

        # Trim the leading (leaf) ISR-dispatch frames from each stack. A stack
        # that consists entirely of dispatch frames would become empty, which
        # renders as a garbage sample downstream — drop its samples instead.
        fully_trimmed: set[int] = set()
        for si, stack in enumerate(d.get("stackTable", [])):
            idx = stack.get("locationIndices", [])
            cut = 0
            while cut < len(idx) and idx[cut] < len(is_isr) and is_isr[idx[cut]]:
                cut += 1
            if cut == len(idx) and idx:
                fully_trimmed.add(si)
            elif cut:
                stack["locationIndices"] = idx[cut:]

        if fully_trimmed:
            for rp in profiles.get("resourceProfiles", []):
                for sp in rp.get("scopeProfiles", []):
                    for p in sp.get("profiles", []):
                        p["samples"] = [
                            smp for smp in p.get("samples", [])
                            if smp.get("stackIndex", 0) not in fully_trimmed
                        ]

        return profiles
