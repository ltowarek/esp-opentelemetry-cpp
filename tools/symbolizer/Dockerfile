# Profiling symbolizer service. Resolves raw Xtensa firmware addresses in OTLP
# profiles against the build ELF using xtensa-esp-elf-addr2line, taken from the
# ESP-IDF toolchain image (the same toolchain that built the firmware).
FROM espressif/idf:release-v6.0 AS toolchain

FROM python:3.12-slim
COPY --from=toolchain /opt/esp/tools/xtensa-esp-elf /opt/xtensa-esp-elf
RUN set -eux; \
    ln -s "$(ls -d /opt/xtensa-esp-elf/*/xtensa-esp-elf/bin)/xtensa-esp-elf-addr2line" \
          /usr/local/bin/xtensa-esp-elf-addr2line; \
    xtensa-esp-elf-addr2line --version | head -1

WORKDIR /app
COPY src/symbolizer /app/symbolizer/

EXPOSE 4319
# python3, not curl/wget: python:3.12-slim doesn't ship either, and this is
# already stdlib-only (see src/symbolizer/symbolizer.py) to keep the image small.
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s \
    CMD ["python3", "-c", "import urllib.request; urllib.request.urlopen('http://localhost:4319/healthz', timeout=2)"]
CMD ["python", "-u", "-m", "symbolizer"]
