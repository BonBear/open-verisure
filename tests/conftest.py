"""Shared setup for the Python script tests: import paths and a fake gateway.

Only public test keys are used (see .github/scripts/check_secrets.py ALLOWED); every
secrets.h/config.h a test needs is written to a temporary directory.
"""
import http.server
import json
import os
import sys
import threading
from urllib.parse import parse_qs, urlparse

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
for p in (ROOT, os.path.join(ROOT, "release"), os.path.join(ROOT, ".github", "scripts")):
    if p not in sys.path:
        sys.path.insert(0, p)

# public CI test master key 00..0f and the device key of 2K79A65A under it
TEST_MASTER_HEX = "000102030405060708090A0B0C0D0E0F"
TEST_DEVICE_ID = "2K79A65A"
TEST_DEVICE_KEY_HEX = "7960FF23E9DFE83C57BF94B437CCC0B9"  # gitleaks:allow (public test vector)
TEST_CHECK_VALUE = "C6A13B"


class _Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802 (http.server API)
        u = urlparse(self.path)
        dev = parse_qs(u.query).get("id", [""])[0]
        status, body = self.server.answer(u.path, dev)
        data = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


class FakeGateway:
    """Answers GET /api/replay?id=<ID> like the gateway: counters maps ID -> counter;
    'override' = (status, body) forces an answer (403, 500, garbage)."""

    def __init__(self):
        self.counters = {}
        self.override = None
        self.requests = []
        self.httpd = http.server.HTTPServer(("127.0.0.1", 0), _Handler)
        self.httpd.answer = self.answer
        self.url = f"http://127.0.0.1:{self.httpd.server_address[1]}"
        self.thread = threading.Thread(target=self.httpd.serve_forever, kwargs={"poll_interval": 0.05},
                                       daemon=True)
        self.thread.start()

    def answer(self, path, dev):
        self.requests.append((path, dev))
        if self.override:
            return self.override
        if path != "/api/replay":
            return 404, "not found"
        if dev in self.counters:
            return 200, json.dumps({"id": dev, "known": True, "counter": self.counters[dev]})
        return 200, json.dumps({"id": dev, "known": False, "counter": 0})

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


@pytest.fixture
def gateway(monkeypatch):
    for v in ("HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy", "ALL_PROXY", "all_proxy"):
        monkeypatch.delenv(v, raising=False)
    monkeypatch.setenv("NO_PROXY", "127.0.0.1")
    g = FakeGateway()
    yield g
    g.close()
