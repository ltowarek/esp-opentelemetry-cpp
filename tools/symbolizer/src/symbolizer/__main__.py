"""Profiling symbolizer: device -> (symbolize) -> collector.

The ESP32-S3 firmware emits OpenTelemetry profiles (OTLP/HTTP JSON) whose
Location addresses are raw Xtensa program counters — it has no symbol table on
board. This service is the device's profiles endpoint: it receives those
profiles, resolves the addresses to function/file/line against the matching
firmware ELF (keyed by build_id = app_elf_sha256) using xtensa-esp-elf-addr2line,
and forwards the enriched profiles to the OpenTelemetry Collector's profiles
pipeline, which exports them to Grafana Pyroscope.

Pyroscope's own symbolizer targets native Linux ELFs and does not resolve
Xtensa, which is why symbolization happens here, host-side, with the toolchain
that built the firmware.

Stdlib only — no third-party dependencies — to keep the image small.
"""

from __future__ import annotations

import json
import logging
import os
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from .symbolizer import Symbolizer

logging.basicConfig(
    level=os.environ.get("SYMBOLIZER_LOG_LEVEL", "INFO"),
    format="%(asctime)s %(levelname)s %(message)s",
)
log = logging.getLogger("symbolizer")

LISTEN_ADDR = os.environ.get("SYMBOLIZER_LISTEN_ADDR", "0.0.0.0:4319")
FORWARD_URL = os.environ.get(
    "SYMBOLIZER_FORWARD_URL",
    "http://otel-collector:4318/v1development/profiles",
)
SYMBOLS_DIR = os.environ.get("SYMBOLIZER_SYMBOLS_DIR", "/symbols")
PROFILES_PATH = "/v1development/profiles"

symbolizer = Symbolizer(SYMBOLS_DIR)


def _forward(payload: bytes) -> tuple[int, bytes]:
    req = urllib.request.Request(
        FORWARD_URL,
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read()
    except (urllib.error.URLError, OSError) as exc:
        # Collector unreachable (restart, DNS, refused) — degrade gracefully
        # instead of crashing the request handler; the device retries next
        # window anyway.
        log.warning("collector unreachable: %s", exc)
        return 502, str(exc).encode()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):  # noqa: D401 - quiet default access log
        return

    def _send(self, status: int, body: bytes = b"") -> None:
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_GET(self) -> None:
        if self.path == "/healthz":
            self._send(200, b'{"status":"ok"}')
        else:
            self._send(404)

    def do_POST(self) -> None:
        if self.path.rstrip("/") != PROFILES_PATH:
            self._send(404)
            return

        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length else b""
        try:
            profiles = json.loads(raw)
        except json.JSONDecodeError as exc:
            log.warning("rejecting non-JSON profiles payload: %s", exc)
            self._send(400, b'{"error":"invalid JSON"}')
            return

        try:
            enriched = symbolizer.symbolize(profiles)
        except Exception:  # noqa: BLE001 - never drop data on a symbolize bug
            log.exception("symbolization failed; forwarding unsymbolized")
            enriched = profiles

        status, body = _forward(json.dumps(enriched).encode())
        if status >= 300:
            log.warning("collector rejected profiles (%d): %s", status, body[:200])
        self._send(status if status < 300 else 502, b'{"status":"forwarded"}')


def main() -> None:
    host, _, port = LISTEN_ADDR.partition(":")
    server = ThreadingHTTPServer((host, int(port)), Handler)
    log.info(
        "profiling-symbolizer listening on %s, forwarding to %s, symbols in %s",
        LISTEN_ADDR,
        FORWARD_URL,
        SYMBOLS_DIR,
    )
    server.serve_forever()


if __name__ == "__main__":
    main()
