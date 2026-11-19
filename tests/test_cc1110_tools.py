"""cc1110_tools.py: config.h helpers, gateway counter seed, wait_for_device, flash_serial."""
import sys
import types
import zlib

import pytest

import cc1110_tools
import derive_key
from conftest import TEST_DEVICE_ID, TEST_DEVICE_KEY_HEX, TEST_MASTER_HEX


# ---------------------------------------------------------------- config.h

def test_config_device_id(tmp_path):
    p = tmp_path / "config.h"
    p.write_text('#define DEVICE_ID "2JHL2MVN"\n', encoding="utf-8")
    assert cc1110_tools.config_device_id(str(p)) == "2JHL2MVN"
    p.write_text('#define DEVICE_ID "XXXX XXXX"\n', encoding="utf-8")  # placeholder, not an ID
    assert cc1110_tools.config_device_id(str(p)) is None
    p.write_text("#define OTHER 1\n", encoding="utf-8")
    assert cc1110_tools.config_device_id(str(p)) is None
    assert cc1110_tools.config_device_id(str(tmp_path / "missing.h")) is None


def test_write_config_without_key_exits(tmp_path, monkeypatch):
    monkeypatch.setattr(derive_key, "SECRETS_H_PATH", str(tmp_path / "none.h"))
    out = tmp_path / "config.h"
    with pytest.raises(SystemExit):
        cc1110_tools.write_config("2JHL 2MVN", str(out))
    assert not out.exists()


def test_write_config_bad_id_exits(tmp_path):
    with pytest.raises(SystemExit):
        cc1110_tools.write_config("2JHL 2MV", str(tmp_path / "config.h"), TEST_MASTER_HEX)


def test_write_config_from_secrets(tmp_path, monkeypatch, capsys):
    s = tmp_path / "secrets.h"
    s.write_text(f'#define MASTER_KEY_HEX "{TEST_MASTER_HEX}"\n', encoding="utf-8")
    monkeypatch.setattr(derive_key, "SECRETS_H_PATH", str(s))
    out = tmp_path / "fw" / "config.h"
    assert cc1110_tools.write_config("2k79-a65a", str(out)) == TEST_DEVICE_ID
    assert cc1110_tools.config_device_id(str(out)) == TEST_DEVICE_ID
    flat = "".join(out.read_text(encoding="utf-8").split()).replace(",", "").replace("0x", "")
    assert TEST_DEVICE_KEY_HEX in flat
    printed = capsys.readouterr().out
    assert TEST_MASTER_HEX not in printed.upper() and "PUBLIC example" not in printed


def test_write_config_example_key_warns(tmp_path, capsys):
    cc1110_tools.write_config(TEST_DEVICE_ID, str(tmp_path / "config.h"), derive_key.DEFAULT_MASTER_KEY_HEX)
    assert "PUBLIC example master key" in capsys.readouterr().out


# ---------------------------------------------------------------- gateway counter

def test_counter_seed_known(gateway):
    gateway.counters[TEST_DEVICE_ID] = 0x1234
    assert cc1110_tools.counter_seed(gateway.url, TEST_DEVICE_ID) == (0x1234 >> 8) + 2
    assert gateway.requests == [("/api/replay", TEST_DEVICE_ID)]


def test_counter_seed_unknown(gateway):
    assert cc1110_tools.counter_seed(gateway.url, TEST_DEVICE_ID) == 1


def test_counter_seed_no_seed_skips_gateway(gateway):
    assert cc1110_tools.counter_seed(gateway.url, TEST_DEVICE_ID, no_seed=True) == 1
    assert gateway.requests == []


BAD_ANSWERS = [
    (403, '{"error":"bad_host"}'),
    (500, "internal error"),
    (200, "<html>not json</html>"),
    (200, '{"known": true}'),
]


@pytest.mark.parametrize("answer", BAD_ANSWERS)
def test_counter_seed_bad_answer_exits(gateway, answer):
    gateway.override = answer
    with pytest.raises(SystemExit) as e:
        cc1110_tools.counter_seed(gateway.url, TEST_DEVICE_ID)
    assert e.value.code == 1


@pytest.mark.parametrize("answer", BAD_ANSWERS)
def test_replay_counter_bad_answer_none(gateway, answer):
    gateway.override = answer
    assert cc1110_tools.replay_counter(gateway.url, TEST_DEVICE_ID) is None


def test_replay_counter_known_unknown(gateway):
    assert cc1110_tools.replay_counter(gateway.url, TEST_DEVICE_ID) == 0
    gateway.counters[TEST_DEVICE_ID] = 777
    assert cc1110_tools.replay_counter(gateway.url, TEST_DEVICE_ID) == 777
    assert cc1110_tools.replay_query(gateway.url, TEST_DEVICE_ID)["known"] is True


def test_counter_seed_gateway_down(gateway):
    url = gateway.url
    gateway.close()  # nothing listens on the port any more
    with pytest.raises(SystemExit):
        cc1110_tools.counter_seed(url, TEST_DEVICE_ID)
    gateway.close = lambda: None


# ---------------------------------------------------------------- wait_for_device

class FakeClock:
    """Stands in for the time module: sleep() advances the clock; time() advances by 'step'
    on every call so that busy loops end."""

    def __init__(self, step=0.0):
        self.t = 1000.0
        self.step = step
        self.slept = 0.0

    def time(self):
        self.t += self.step
        return self.t

    def sleep(self, d):
        self.t += d
        self.slept += d


@pytest.fixture
def clock(monkeypatch):
    c = FakeClock()
    monkeypatch.setattr(cc1110_tools, "time", c)
    return c


def _counters(monkeypatch, values):
    it = iter(values)
    calls = []

    def fake(gw, dev):
        calls.append(dev)
        return next(it)
    monkeypatch.setattr(cc1110_tools, "replay_counter", fake)
    return calls


def test_wait_for_device_reaches_seed_block(monkeypatch, clock):
    seed = 5
    calls = _counters(monkeypatch, [None, (seed << 8) - 1, seed << 8])
    assert cc1110_tools.wait_for_device("http://gw", TEST_DEVICE_ID, seed, timeout=90)
    assert len(calls) == 3


def test_wait_for_device_above_seed(monkeypatch, clock):
    _counters(monkeypatch, [(5 << 8) + 300])
    assert cc1110_tools.wait_for_device("http://gw", TEST_DEVICE_ID, 5, timeout=90)
    assert clock.slept == 0


def test_wait_for_device_timeout_below(monkeypatch, clock, capsys):
    calls = _counters(monkeypatch, [(5 << 8) - 1] * 100)
    assert not cc1110_tools.wait_for_device("http://gw", TEST_DEVICE_ID, 5, timeout=10)
    assert len(calls) == 5  # one poll every 2 s
    assert "No authenticated packet" in capsys.readouterr().out


def test_wait_for_device_timeout_no_gateway(monkeypatch, clock, capsys):
    _counters(monkeypatch, [None] * 100)
    assert not cc1110_tools.wait_for_device("http://gw", TEST_DEVICE_ID, 5, timeout=10)
    assert "did not answer" in capsys.readouterr().out


def test_wait_for_device_with_fake_gateway(gateway, clock):
    gateway.counters[TEST_DEVICE_ID] = 3 << 8
    assert cc1110_tools.wait_for_device(gateway.url, TEST_DEVICE_ID, 3, timeout=5)
    assert not cc1110_tools.wait_for_device(gateway.url, TEST_DEVICE_ID, 4, timeout=5)


# ---------------------------------------------------------------- flash_serial

class FakeSerial:
    """Fake pyserial port for the ESP_CC_Flasher serial protocol. 'script' maps a step (PING,
    INIT, FLASH, DATA for the binary, DONE for what follows #GOT) to the lines it answers; an
    empty entry answers nothing, so the caller times out."""

    instances = []
    script = {}

    def __init__(self):
        self.written = []
        self.lines = []
        self.is_open = False
        self.dtr = self.rts = True
        self.dtr_at_open = self.rts_at_open = None
        FakeSerial.instances.append(self)

    def open(self):
        self.dtr_at_open, self.rts_at_open = self.dtr, self.rts
        self.is_open = True

    def close(self):
        self.is_open = False

    def reset_input_buffer(self):
        self.lines = []

    def _queue(self, step):
        self.lines += [(line + "\n").encode() for line in self.script.get(step, [])]

    def write(self, b):
        assert self.is_open
        self.written.append(bytes(b))
        word = bytes(b).split(b" ")[0].strip()
        if word in (b"PING", b"INIT", b"FLASH"):
            self._queue(word.decode())
        else:
            self._queue("DATA")
            if self.script.get("DATA") == ["#GOT"]:
                self._queue("DONE")

    def readline(self):
        return self.lines.pop(0) if self.lines else b""


GOOD = {
    "PING": ["#PONG"],
    "INIT": ["debug: target found", "#INIT 0104 00"],
    "FLASH": ["#READY"],
    "DATA": ["#GOT"],
    "DONE": ["erasing...", "#DONE OK"],
}


@pytest.fixture
def port(monkeypatch, tmp_path):
    FakeSerial.instances = []
    FakeSerial.script = dict(GOOD)
    monkeypatch.setitem(sys.modules, "serial", types.SimpleNamespace(Serial=FakeSerial))
    monkeypatch.setattr(cc1110_tools, "time", FakeClock(step=0.25))
    binp = tmp_path / "fw.bin"
    binp.write_bytes(bytes(range(256)) * 4)
    return FakeSerial, str(binp)


def test_flash_serial_ok(port, capsys):
    fs, binp = port
    cc1110_tools.flash_serial("COM_TEST", binp)
    s = fs.instances[0]
    data = open(binp, "rb").read()
    assert s.port == "COM_TEST" and s.baudrate == 115200
    assert s.dtr_at_open is False and s.rts_at_open is False  # opening must not reset the ESP32
    assert not s.is_open
    assert s.written[:2] == [b"PING\n", b"INIT\n"]
    assert s.written[2] == f"FLASH {len(data)} {zlib.crc32(data):08x}\n".encode()
    assert s.written[3] == data
    assert "SUCCEEDED" in capsys.readouterr().out


@pytest.mark.parametrize("step,lines", [
    ("PING", []),                       # no answer
    ("PING", ["#ERR unknown"]),         # other firmware
    ("INIT", ["#INIT 0000 00"]),        # no CC1110F32
    ("INIT", ["#INIT 0104"]),           # malformed
    ("INIT", []),                       # timeout
    ("FLASH", ["#ERR busy"]),
    ("DATA", ["#ERR CRC"]),
    ("DATA", []),
    ("DONE", ["#DONE VERIFY_FAIL"]),
    ("DONE", []),
])
def test_flash_serial_failures(port, step, lines):
    fs, binp = port
    fs.script[step] = lines
    with pytest.raises(SystemExit) as e:
        cc1110_tools.flash_serial("COM_TEST", binp)
    assert e.value.code == 1
    assert not fs.instances[0].is_open


def test_flash_serial_locked_refused(port, capsys):
    fs, binp = port
    fs.script["INIT"] = ["#INIT 0104 04"]
    with pytest.raises(SystemExit):
        cc1110_tools.flash_serial("COM_TEST", binp)
    assert not any(w.startswith(b"FLASH") for w in fs.instances[0].written)
    assert "debug-locked" in capsys.readouterr().out


def test_flash_serial_locked_allowed(port, capsys):
    fs, binp = port
    fs.script["INIT"] = ["#INIT 0104 06"]
    cc1110_tools.flash_serial("COM_TEST", binp, allow_locked=True)
    out = capsys.readouterr().out
    assert "--erase" in out and "SUCCEEDED" in out
