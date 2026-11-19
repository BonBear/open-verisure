"""868Receiver/ota_upload.py: the local image check and the password sources."""
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "868Receiver"))
import ota_upload  # noqa: E402

# the first 36 bytes of a real ESP32-S2 firmware.bin of the gateway (see test_ota_image.cpp)
GOOD = bytes.fromhex("e906022f5c560240ee0000000200000000ffff0000000001" "2000003fc8ae0200" "3254cdab")


def test_accepts_s2_app():
    assert ota_upload.check_app_header(GOOD) is None
    assert ota_upload.check_app_header(GOOD + b"\0" * 100) is None


@pytest.mark.parametrize("pos,val,reason", [
    (0, 0xE8, "magic"),
    (1, 0, "segment"),
    (1, 17, "segment"),
    (12, 0, "another chip"),
    (32, 0, "app description"),
])
def test_rejects(pos, val, reason):
    h = bytearray(GOOD)
    h[pos] = val
    assert reason in ota_upload.check_app_header(bytes(h))


def test_too_short():
    assert ota_upload.check_app_header(GOOD[:35]) == "image too short"


def test_password_file_first_line(tmp_path, monkeypatch):
    monkeypatch.setenv("GATEWAY_PASS", "from-env")
    p = tmp_path / "pw.txt"
    p.write_text("secret pw\r\nsecond line\n", encoding="utf-8")
    assert ota_upload.read_password(str(p)) == "secret pw"     # the file wins over the env


def test_password_env(monkeypatch):
    monkeypatch.setenv("GATEWAY_PASS", "from-env")
    assert ota_upload.read_password(None) == "from-env"
    monkeypatch.delenv("GATEWAY_PASS")
    assert ota_upload.read_password(None) is None


def test_no_password_exits_without_upload(monkeypatch, capsys):
    monkeypatch.delenv("GATEWAY_PASS", raising=False)
    assert ota_upload.main(["--host", "127.0.0.1", "--bin", "nonexistent.bin"]) == 2
    assert "GATEWAY_PASS" in capsys.readouterr().out
