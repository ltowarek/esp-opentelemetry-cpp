"""Tests for the symbolizer's HTTP endpoint, against a real server on localhost."""

import threading
import urllib.error
import urllib.request
from collections.abc import Iterator
from http.server import ThreadingHTTPServer

import pytest

from symbolizer.__main__ import PROFILES_PATH, Handler


@pytest.fixture
def server_url() -> Iterator[str]:
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield f"http://127.0.0.1:{server.server_address[1]}{PROFILES_PATH}"
    server.shutdown()
    server.server_close()


def _post_status(url: str, body: bytes) -> int:
    request = urllib.request.Request(
        url, data=body, headers={"Content-Type": "application/json"}, method="POST"
    )
    try:
        with urllib.request.urlopen(request, timeout=5) as response:
            return response.status
    except urllib.error.HTTPError as exc:
        return exc.code


def test_body_that_is_not_utf8_is_rejected(server_url: str) -> None:
    assert _post_status(server_url, b'{"name": "bad\xff"}') == 400
