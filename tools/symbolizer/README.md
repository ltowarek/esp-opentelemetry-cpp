# profiling symbolizer

Host-side service that turns the device's **unsymbolized** OTLP profiles into
symbolized ones. The firmware has no symbol table on board, and profile
backends do not symbolize Xtensa ELFs, so symbolization happens here — with
the same toolchain that built the firmware.

Pipeline position:

```
device --OTLP/JSON profiles (raw addrs + build_id)--> symbolizer
    -> xtensa-esp-elf-addr2line against the matching ELF
    -> OTLP collector `profiles` pipeline -> profile backend (e.g. Pyroscope)
```

The profile `build_id` equals the firmware ELF's sha256 (`app_elf_sha256`), so
ELFs are **auto-discovered by hash** from mounted build directories — no
publish step. An archived build can be symbolized by placing its ELF in the
symbols directory as `<sha256>.elf`. Leading timer-ISR dispatch frames are
trimmed from every stack so the interrupted application code is the
flame-graph leaf; samples whose stack consists entirely of dispatch frames are
dropped.

## Configuration (environment)

| Variable | Default | Description |
|----------|---------|-------------|
| `SYMBOLIZER_LISTEN_ADDR` | `0.0.0.0:4319` | OTLP/HTTP JSON profiles listener (the device's `CONFIG_ESP_OPENTELEMETRY_PROFILES_OTLP_BASE_URL`) |
| `SYMBOLIZER_FORWARD_URL` | `http://otel-collector:4318/v1development/profiles` | Where symbolized profiles are forwarded |
| `SYMBOLIZER_SYMBOLS_DIR` | `/symbols` | Manual-override store (`<sha256>.elf`) |
| `SYMBOLIZER_ELF_GLOBS` | `/firmware/build/*.elf:/firmware/components/*/test_apps/build/*.elf` | Colon-separated glob patterns for ELF auto-discovery |
| `SYMBOLIZER_TRIM_REGEX` | this component's sampler + gptimer/Xtensa dispatch | Override for apps with custom samplers |
| `SYMBOLIZER_ADDR2LINE` | `xtensa-esp-elf-addr2line` | Resolver binary |

## Run

The Dockerfile copies `xtensa-esp-elf-addr2line` out of the `espressif/idf`
image so the resolver always matches the firmware toolchain. Compose sketch:

```yaml
profiling-symbolizer:
  build:
    context: <path-to>/esp-opentelemetry-cpp/tools/symbolizer
  ports:
    - "4319:4319"
  volumes:
    - <path-to-firmware-project>:/firmware:ro
    - ./symbols:/symbols:ro
```

The collector needs a `profiles` pipeline and the
`--feature-gates=service.profilesSupport` flag (the OTLP profiles signal is
still in development upstream).

## Tests

```sh
python -m pytest tools/symbolizer
```

Resolution is stubbed in the tests, so no toolchain or ELF is needed.
