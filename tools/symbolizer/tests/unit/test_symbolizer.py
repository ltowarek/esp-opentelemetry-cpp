"""Unit tests for the profiling symbolizer transform.

Resolution (addr2line) is stubbed so the tests run without a toolchain or ELF;
they cover build_id extraction, function/line table construction, and the
ISR-frame trimming that turns sampler/ISR plumbing into clean application
leaves.
"""

from symbolizer.symbolizer import Symbolizer


def _profile(build_id="abc"):
    # Stacks are leaf-first. Stack 0: [on_timer, gptimer_default_isr, _xt_lowint1,
    # prof_hot_spin, busy_task] -> the first three are ISR frames to trim.
    return {
        "dictionary": {
            "stringTable": ["", "samples", "count", "process.executable.build_id"],
            "functionTable": [],
            "mappingTable": [{"attributeIndices": [0]}],
            "locationTable": [
                {"address": "1"},  # on_timer
                {"address": "2"},  # gptimer_default_isr
                {"address": "3"},  # _xt_lowint1
                {"address": "4"},  # prof_hot_spin
                {"address": "5"},  # busy_task
            ],
            "stackTable": [{"locationIndices": [0, 1, 2, 3, 4]}],
            "attributeTable": [
                {"keyStrindex": 3, "value": {"stringValue": build_id}}
            ],
        }
    }


# Function names as xtensa-esp-elf-addr2line -C actually emits them.
_RESOLUTION = {
    1: ("(anonymous namespace)::on_timer(gptimer_t*, void*)", "profiling.cpp", 95),
    2: ("gptimer_default_isr", "gptimer.c", 465),
    3: ("_xt_lowint1", "xtensa_vectors.S", 1264),
    4: ("(anonymous namespace)::prof_hot_spin()", "test.cpp", 20),
    5: ("(anonymous namespace)::busy_task(void*)", "test.cpp", 30),
}


class _StubSymbolizer(Symbolizer):
    def __init__(self, tmp_path):
        super().__init__(str(tmp_path))
        self._addr2line = "stub"  # bypass shutil.which gate

    def _resolve(self, elf, addresses):
        return {a: _RESOLUTION[a] for a in addresses}


def test_build_id_extraction():
    s = Symbolizer("/nonexistent", elf_globs="")
    assert s._build_id(_profile("deadbeef")["dictionary"]) == "deadbeef"


def test_elf_auto_discovery_by_hash(tmp_path):
    import hashlib

    build = tmp_path / "build"
    build.mkdir()
    elf = build / "app.elf"
    elf.write_bytes(b"\x7fELF-fake-firmware")
    build_id = hashlib.sha256(elf.read_bytes()).hexdigest()

    s = Symbolizer(str(tmp_path / "symbols"), elf_globs=str(build / "*.elf"))
    assert s.elf_for_build_id(build_id) == str(elf)
    assert s.elf_for_build_id("0" * 64) is None

    # A rebuilt ELF (same path, new content) is re-hashed and found again.
    elf.write_bytes(b"\x7fELF-fake-firmware-v2")
    new_id = hashlib.sha256(elf.read_bytes()).hexdigest()
    assert s.elf_for_build_id(new_id) == str(elf)


def test_symbols_dir_overrides_discovery(tmp_path):
    import hashlib

    elf = tmp_path / "app.elf"
    elf.write_bytes(b"\x7fELF")
    build_id = hashlib.sha256(elf.read_bytes()).hexdigest()
    symbols = tmp_path / "symbols"
    symbols.mkdir()
    (symbols / f"{build_id}.elf").write_bytes(b"\x7fELF")

    s = Symbolizer(str(symbols), elf_globs=str(tmp_path / "*.elf"))
    assert s.elf_for_build_id(build_id) == str(symbols / f"{build_id}.elf")


def test_symbolize_builds_functions_and_trims_isr(tmp_path):
    (tmp_path / "abc.elf").write_bytes(b"\x7fELF")  # presence is all that matters
    out = _StubSymbolizer(tmp_path).symbolize(_profile("abc"))
    d = out["dictionary"]

    # Leading ISR frames trimmed; application code becomes the leaf.
    names = [
        d["stringTable"][d["functionTable"][loc["lines"][0]["functionIndex"]]["nameStrindex"]]
        for loc in (d["locationTable"][i] for i in d["stackTable"][0]["locationIndices"])
    ]
    assert names == [
        "(anonymous namespace)::prof_hot_spin()",
        "(anonymous namespace)::busy_task(void*)",
    ]


def test_no_elf_forwards_unsymbolized(tmp_path):
    out = _StubSymbolizer(tmp_path).symbolize(_profile("missing"))
    assert out["dictionary"]["functionTable"] == []


def test_fully_trimmed_stack_drops_its_samples(tmp_path):
    (tmp_path / "abc.elf").write_bytes(b"\x7fELF")
    p = _profile("abc")
    d = p["dictionary"]
    # Stack 1 consists solely of ISR-dispatch frames (locations 0-2).
    d["stackTable"].append({"locationIndices": [0, 1, 2]})
    p["resourceProfiles"] = [{
        "scopeProfiles": [{
            "profiles": [{
                "samples": [
                    {"stackIndex": 0, "values": ["5"]},
                    {"stackIndex": 1, "values": ["3"]},
                ],
            }],
        }],
    }]
    out = _StubSymbolizer(tmp_path).symbolize(p)
    samples = out["resourceProfiles"][0]["scopeProfiles"][0]["profiles"][0]["samples"]
    assert [smp["stackIndex"] for smp in samples] == [0]
