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
        if word in (b"PING", b"INIT", b"FLASH", b"READ"):
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


# ---------------------------------------------------------------- parse_ctr_log

def _ctr_entry(block: int) -> bytes:
    zeros = 24 - bin(block).count("1")
    return bytes([block & 0xFF, zeros, (block >> 8) & 0xFF, (block >> 16) & 0xFF])


def _make_ctr_log(page0=(), page1=()) -> bytes:
    buf = bytearray(b"\xff" * cc1110_tools.CTR_LOG_SIZE)
    for idx, entry in enumerate(page0):
        buf[idx * 4:idx * 4 + 4] = entry
    for idx, entry in enumerate(page1):
        off = 1024 + idx * 4
        buf[off:off + 4] = entry
    return bytes(buf)


def test_parse_ctr_log_erased_and_zero():
    assert cc1110_tools.parse_ctr_log(b"\xff" * cc1110_tools.CTR_LOG_SIZE) is None
    assert cc1110_tools.parse_ctr_log(b"\x00" * cc1110_tools.CTR_LOG_SIZE) is None


def test_parse_ctr_log_single_entry():
    log = _make_ctr_log(page0=[_ctr_entry(42)])
    assert cc1110_tools.parse_ctr_log(log) == 42


def test_parse_ctr_log_rollover_both_directions():
    # Page 1 newer than page 0
    log_p1_newer = _make_ctr_log(
        page0=[_ctr_entry(10), _ctr_entry(11)],
        page1=[_ctr_entry(12), _ctr_entry(15)],
    )
    assert cc1110_tools.parse_ctr_log(log_p1_newer) == 15

    # Rollover back to page 0: page 0 has higher block than page 1
    log_p0_newer = _make_ctr_log(
        page0=[_ctr_entry(500), _ctr_entry(505)],
        page1=[_ctr_entry(498), _ctr_entry(499)],
    )
    assert cc1110_tools.parse_ctr_log(log_p0_newer) == 505


def test_parse_ctr_log_torn_write_rejected():
    valid = _ctr_entry(0x010203)  # b0=0x03, check=20 (0x14), b1=0x02, b2=0x01
    # 1-bit left at 1 in block byte b0 (0 -> 1)
    torn_block = bytes([valid[0] | 0x04, valid[1], valid[2], valid[3]])
    # 1-bit left at 1 in check byte (0 -> 1)
    torn_check = bytes([valid[0], valid[1] | 0x01, valid[2], valid[3]])
    log = _make_ctr_log(page0=[_ctr_entry(7), torn_block, torn_check])
    assert cc1110_tools.parse_ctr_log(log) == 7
    assert cc1110_tools.parse_ctr_log(_make_ctr_log(page0=[torn_block, torn_check])) is None


def test_parse_ctr_log_b2_ff_rejected_even_if_check_matches():
    # 0xFF0000 has 16 zero bits; check == 16 matches, yet b2 == 0xFF must be rejected
    fake_ff = bytes([0x00, 16, 0x00, 0xFF])
    assert cc1110_tools.parse_ctr_log(_make_ctr_log(page0=[fake_ff])) is None
    assert cc1110_tools.parse_ctr_log(_make_ctr_log(page0=[_ctr_entry(9), fake_ff])) == 9


@pytest.mark.parametrize("bad_len", [0, 4, cc1110_tools.CTR_LOG_SIZE - 1, cc1110_tools.CTR_LOG_SIZE + 1])
def test_parse_ctr_log_wrong_length_raises(bad_len):
    with pytest.raises(ValueError):
        cc1110_tools.parse_ctr_log(b"\xff" * bad_len)


# ---------------------------------------------------------------- read_flash_serial

def _read_script_for(payload: bytes, chunk_bytes: int = 32):
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    hex_lines = [payload[i:i + chunk_bytes].hex() for i in range(0, len(payload), chunk_bytes)]
    return {
        "PING": ["#PONG"],
        "READ": [f"#DATA {len(payload)} {crc:08x}", *hex_lines, "#DONE OK"],
    }


def test_read_flash_serial_ok(port):
    fs, _ = port
    payload = _make_ctr_log(page0=[_ctr_entry(123)], page1=[_ctr_entry(456)])
    fs.script = _read_script_for(payload)
    got = cc1110_tools.read_flash_serial("COM_TEST", cc1110_tools.CTR_LOG_ADDR, cc1110_tools.CTR_LOG_SIZE)
    assert got == payload
    s = fs.instances[0]
    assert s.port == "COM_TEST" and s.baudrate == 115200 and s.timeout == 0.5
    assert s.dtr_at_open is False and s.rts_at_open is False
    assert not s.is_open
    assert s.written == [b"PING\n", b"READ 7800 2048\n"]


def test_read_flash_serial_bad_crc(port):
    fs, _ = port
    payload = b"\xAA" * 64
    fs.script = {
        "PING": ["#PONG"],
        "READ": [f"#DATA {len(payload)} 00000000", payload.hex(), "#DONE OK"],
    }
    assert cc1110_tools.read_flash_serial("COM_TEST", 0x7800, 64) is None
    assert not fs.instances[0].is_open


def test_read_flash_serial_wrong_or_truncated_length(port):
    fs, _ = port
    payload = b"\x55" * 64
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    # Header announces wrong length
    fs.script = {"PING": ["#PONG"], "READ": [f"#DATA 32 {crc:08x}", payload[:32].hex(), "#DONE OK"]}
    assert cc1110_tools.read_flash_serial("COM_TEST", 0x7800, 64) is None
    assert not fs.instances[-1].is_open

    # Payload shorter than announced length
    short = payload[:32]
    fs.script = {
        "PING": ["#PONG"],
        "READ": [f"#DATA 64 {zlib.crc32(short) & 0xFFFFFFFF:08x}", short.hex(), "#DONE OK"],
    }
    assert cc1110_tools.read_flash_serial("COM_TEST", 0x7800, 64) is None
    assert not fs.instances[-1].is_open


@pytest.mark.parametrize("err_line", [
    "#ERR unknown command",
    "#ERR locked",
    "#ERR no_target",
    "#ERR other",
    "#DATA malformed",
    "#DATA abc 00000000",
])
def test_read_flash_serial_err_responses(port, err_line):
    fs, _ = port
    fs.script = {"PING": ["#PONG"], "READ": [err_line]}
    assert cc1110_tools.read_flash_serial("COM_TEST", 0x7800, 64) is None
    s = fs.instances[0]
    assert s.dtr_at_open is False and s.rts_at_open is False
    assert not s.is_open


def test_read_flash_serial_ping_timeout(port):
    fs, _ = port
    fs.script = {"PING": []}
    assert cc1110_tools.read_flash_serial("COM_TEST", 0x7800, 64) is None
    assert not fs.instances[0].is_open


def test_read_flash_serial_missing_done_timeout(port):
    fs, _ = port
    payload = b"\x11" * 64
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    fs.script = {"PING": ["#PONG"], "READ": [f"#DATA 64 {crc:08x}", payload.hex()]}
    assert cc1110_tools.read_flash_serial("COM_TEST", 0x7800, 64) is None
    assert not fs.instances[0].is_open


def test_read_flash_serial_invalid_hex(port):
    fs, _ = port
    fs.script = {"PING": ["#PONG"], "READ": ["#DATA 4 00000000", "zzzzzzzz", "#DONE OK"]}
    assert cc1110_tools.read_flash_serial("COM_TEST", 0x7800, 4) is None
    assert not fs.instances[0].is_open


def test_read_flash_serial_open_oserror(monkeypatch):
    class FailSerial(FakeSerial):
        def open(self):
            raise OSError("port busy")

    FakeSerial.instances = []
    monkeypatch.setitem(sys.modules, "serial", types.SimpleNamespace(Serial=FailSerial))
    assert cc1110_tools.read_flash_serial("COM_BUSY", 0x7800, 64) is None
    assert not FakeSerial.instances[0].is_open


# ---------------------------------------------------------------- counter_seed + serial_port

def test_counter_seed_device_higher_than_gateway(gateway, port, capsys):
    fs, _ = port
    gateway.counters[TEST_DEVICE_ID] = 10 << 8  # gw_seed = 12
    log = _make_ctr_log(page0=[_ctr_entry(20)])  # dev_seed = 23
    fs.script = _read_script_for(log)
    seed = cc1110_tools.counter_seed(gateway.url, TEST_DEVICE_ID, serial_port="COM_TEST")
    assert seed == 23
    assert "above gateway" in capsys.readouterr().out


def test_counter_seed_gateway_higher_than_device(gateway, port, capsys):
    fs, _ = port
    gateway.counters[TEST_DEVICE_ID] = 50 << 8  # gw_seed = 52
    log = _make_ctr_log(page0=[_ctr_entry(20)])  # dev_seed = 23
    fs.script = _read_script_for(log)
    seed = cc1110_tools.counter_seed(gateway.url, TEST_DEVICE_ID, serial_port="COM_TEST")
    assert seed == 52
    assert "gateway seed 52 is higher" in capsys.readouterr().out


def test_counter_seed_unknown_gateway_uses_device_log(gateway, port):
    fs, _ = port
    log = _make_ctr_log(page1=[_ctr_entry(17)])  # dev_seed = 20
    fs.script = _read_script_for(log)
    seed = cc1110_tools.counter_seed(gateway.url, TEST_DEVICE_ID, serial_port="COM_TEST")
    assert seed == 20


@pytest.mark.parametrize("err_line", ["#ERR unknown command", "#ERR locked"])
def test_counter_seed_flasher_fallback_to_gateway(gateway, port, err_line):
    fs, _ = port
    gateway.counters[TEST_DEVICE_ID] = 8 << 8  # gw_seed = 10
    fs.script = {"PING": ["#PONG"], "READ": [err_line]}
    seed = cc1110_tools.counter_seed(gateway.url, TEST_DEVICE_ID, serial_port="COM_TEST")
    assert seed == 10


def test_counter_seed_no_seed_skips_serial_and_gateway(gateway, port):
    fs, _ = port
    seed = cc1110_tools.counter_seed(gateway.url, TEST_DEVICE_ID, no_seed=True, serial_port="COM_TEST")
    assert seed == 1
    assert gateway.requests == []
    assert fs.instances == []


def test_counter_seed_exceeds_block_max_exits(gateway, port):
    fs, _ = port
    log = _make_ctr_log(page0=[_ctr_entry(cc1110_tools.CTR_BLOCK_MAX - 2)])  # + 3 > CTR_BLOCK_MAX
    fs.script = _read_script_for(log)
    with pytest.raises(SystemExit) as e:
        cc1110_tools.counter_seed(gateway.url, TEST_DEVICE_ID, serial_port="COM_TEST")
    assert e.value.code == 1

    # Exactly CTR_BLOCK_MAX is still allowed
    log_ok = _make_ctr_log(page0=[_ctr_entry(cc1110_tools.CTR_BLOCK_MAX - 3)])
    fs.script = _read_script_for(log_ok)
    assert cc1110_tools.counter_seed(gateway.url, TEST_DEVICE_ID, serial_port="COM_TEST") == cc1110_tools.CTR_BLOCK_MAX
