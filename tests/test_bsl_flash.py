"""Unit tests for tools/esp32_msp430_bsl/bsl_flash.py."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest


def _load_bsl_flash():
    path = Path(__file__).resolve().parents[1] / "tools" / "esp32_msp430_bsl" / "bsl_flash.py"
    spec = importlib.util.spec_from_file_location("bsl_flash", path)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules["bsl_flash"] = mod
    spec.loader.exec_module(mod)
    return mod


bsl_flash = _load_bsl_flash()


def _make_rx_frame(payload: bytes) -> bytes:
    header = bytes([0x80, 0x00, len(payload), len(payload)]) + payload
    ckl, ckh = bsl_flash.bsl_checksum(header)
    return header + bytes([ckl, ckh])


# --- TI SLAU319 checksum and frame vectors ---


@pytest.mark.parametrize(
    ("cmd", "addr", "param", "payload", "expected_hex"),
    [
        # 1. TX BSL Version (0x1E)
        (0x1E, 0x0000, 0x0000, b"", "801E0404000000007BE5"),
        # 2. Mass Erase (0x18, 0xFF00, 0xA506)
        (0x18, 0xFF00, 0xA506, b"", "8018040400FF06A57DB9"),
        # 3. RX Password (0x10, 32x 0xFF)
        (0x10, 0x0000, 0x0000, b"\xff" * 32, "8010242400000000" + ("FF" * 32) + "5BCB"),
        # 4. RX Data Block (0x12 at 0x1100, 2-byte payload)
        (0x12, 0x1100, 0x0002, b"\x12\x34", "8012060600110200123469CE"),
        # 5. TX Data Block (0x14 at 0xFFFE, length 2)
        (0x14, 0xFFFE, 0x0002, b"", "80140404FEFF02008710"),
    ],
)
def test_slau319_frame_vectors(cmd, addr, param, payload, expected_hex):
    frame = bsl_flash.build_frame(cmd, addr, param, payload)
    assert frame == bytes.fromhex(expected_hex)


def test_bsl_checksum_validation():
    assert bsl_flash.bsl_checksum(bytes.fromhex("801E040400000000")) == (0x7B, 0xE5)
    with pytest.raises(ValueError):
        bsl_flash.bsl_checksum(b"")
    with pytest.raises(ValueError):
        bsl_flash.bsl_checksum(b"\x80\x1e\x04")


def test_build_frame_rejects_bad_payload():
    with pytest.raises(ValueError):
        bsl_flash.build_frame(0x12, 0x1100, 1, b"\xaa")
    with pytest.raises(ValueError):
        bsl_flash.build_frame(0x12, 0x1100, 242, b"\xaa" * 242)


# --- verify_rx_frame ---


def test_verify_rx_frame_valid():
    raw = bytes.fromhex("80000202AABBD746")
    assert bsl_flash.verify_rx_frame(raw, 2) == b"\xaa\xbb"


def test_verify_rx_frame_rejects_corrupt_frames():
    valid = bytearray.fromhex("80000202AABBD746")

    bad_hdr0 = bytearray(valid)
    bad_hdr0[0] = 0x81
    with pytest.raises(ValueError):
        bsl_flash.verify_rx_frame(bytes(bad_hdr0), 2)

    bad_hdr1 = bytearray(valid)
    bad_hdr1[1] = 0x01
    with pytest.raises(ValueError):
        bsl_flash.verify_rx_frame(bytes(bad_hdr1), 2)

    bad_len = bytearray(valid)
    bad_len[3] = 0x04
    with pytest.raises(ValueError):
        bsl_flash.verify_rx_frame(bytes(bad_len), 2)

    with pytest.raises(ValueError):
        bsl_flash.verify_rx_frame(bytes(valid), 4)

    bad_cksum = bytearray(valid)
    bad_cksum[-1] ^= 0x01
    with pytest.raises(ValueError):
        bsl_flash.verify_rx_frame(bytes(bad_cksum), 2)


# --- Intel HEX & TI-TXT parsers ---


def test_parse_ihex_valid_and_extended_linear():
    ihex = (
        ":021100001234A7\n"
        ":020000040000FA\n"
        ":02FFFE000011F0\n"
        ":020000040001F9\n"
        ":01000000AA55\n"
        ":00000001FF\n"
    )
    mem = bsl_flash.parse_ihex(ihex)
    assert mem == {
        0x1100: 0x12,
        0x1101: 0x34,
        0xFFFE: 0x00,
        0xFFFF: 0x11,
        0x10000: 0xAA,
    }


def test_parse_ihex_rejects_bad_checksum():
    bad_ihex = (
        ":021100001234A8\n"
        ":00000001FF\n"
    )
    with pytest.raises(ValueError, match="checksum"):
        bsl_flash.parse_ihex(bad_ihex)


def test_parse_titxt_multi_section():
    titxt = (
        "@1100\n"
        "12 34 56\n"
        "@FFFE\n"
        "00 11\n"
        "q\n"
    )
    mem = bsl_flash.parse_titxt(titxt)
    assert mem == {
        0x1100: 0x12,
        0x1101: 0x34,
        0x1102: 0x56,
        0xFFFE: 0x00,
        0xFFFF: 0x11,
    }


# --- prepare_flash_chunks ---


def test_prepare_flash_chunks_alignment_and_padding():
    mem = {
        0x1101: 0x12,
        0x1102: 0x34,
        0xFFFE: 0x00,
        0xFFFF: 0x11,
    }
    chunks = bsl_flash.prepare_flash_chunks(mem)
    assert chunks == [
        (0x1100, b"\xff\x12\x34\xff"),
        (0xFFFE, b"\x00\x11"),
    ]


def test_prepare_flash_chunks_splits_default_96_and_skips_blank():
    mem = {0x1100 + i: 0xFF for i in range(96)}
    for i in range(500):
        mem[0x1200 + i] = i & 0xFF
    mem[0xFFFE] = 0x00
    mem[0xFFFF] = 0x11

    chunks = bsl_flash.prepare_flash_chunks(mem)
    assert [(addr, len(data)) for addr, data in chunks] == [
        (0x1200, 96),
        (0x1260, 96),
        (0x12C0, 96),
        (0x1320, 96),
        (0x1380, 96),
        (0x13E0, 20),
        (0xFFFE, 2),
    ]

    chunks_240 = bsl_flash.prepare_flash_chunks(mem, max_chunk=240)
    assert [(addr, len(data)) for addr, data in chunks_240] == [
        (0x1200, 240),
        (0x12F0, 240),
        (0x13E0, 20),
        (0xFFFE, 2),
    ]


@pytest.mark.parametrize("bad_addr", [0x0200, 0x0C00, 0x0FFF])
def test_prepare_flash_chunks_rejects_forbidden_ranges(bad_addr):
    mem = {bad_addr: 0xAA, 0xFFFE: 0x00, 0xFFFF: 0x11}
    with pytest.raises(ValueError, match="outside MSP430F149 flash"):
        bsl_flash.prepare_flash_chunks(mem)


def test_prepare_flash_chunks_info_flash_policy():
    mem = {0x1000: 0xAA, 0x1001: 0xBB, 0xFFFE: 0x00, 0xFFFF: 0x11}
    with pytest.raises(ValueError, match="Info Flash"):
        bsl_flash.prepare_flash_chunks(mem, allow_info=False)

    chunks = bsl_flash.prepare_flash_chunks(mem, allow_info=True)
    assert chunks == [(0x1000, b"\xaa\xbb"), (0xFFFE, b"\x00\x11")]


def test_prepare_flash_chunks_reset_vector_required():
    with pytest.raises(ValueError, match="Reset vector"):
        bsl_flash.prepare_flash_chunks({0x1100: 0xAA, 0x1101: 0xBB})

    with pytest.raises(ValueError, match="blank"):
        bsl_flash.prepare_flash_chunks({0x1100: 0xAA, 0xFFFE: 0xFF, 0xFFFF: 0xFF})

    chunks = bsl_flash.prepare_flash_chunks(
        {0x1100: 0xAA, 0x1101: 0xBB},
        allow_no_reset_vec=True,
    )
    assert chunks == [(0x1100, b"\xaa\xbb")]


# --- Mocked BslBridge tests ---


class FakeBridgeSerial:
    """In-memory mock of the ESP32-C3 BSL bridge serial interface."""

    def __init__(
        self,
        fail_cmd: int | None = None,
        info_fill: int = 0xFF,
        locked: bool = True,
        bsl_ver: int = 0x0160,
        stub_status: int = 0xCAFE,
        stub_statuses: list[int] | None = None,
        corrupt_readback_once_addr: int | None = None,
        probe_err: str = "ERR SHORT A0",
    ) -> None:
        self.commands: list[str] = []
        self.xfer_frames: list[bytes] = []
        self.flash: dict[int, int] = {a: 0xFF for a in range(0x1100, 0x10000)}
        for a in range(0x1000, 0x1100):
            self.flash[a] = info_fill
        self.ram: dict[int, int] = {a: 0x00 for a in range(0x0200, 0x0A00)}
        self.rom_id = bytes(
            [0xF1, 0x49] + [0x00] * 8 + [(bsl_ver >> 8) & 0xFF, bsl_ver & 0xFF] + [0x00] * 4
        )
        self.fail_cmd = fail_cmd
        self.locked = locked
        self.bsl_ver = bsl_ver
        self.stub_status = stub_status
        self.stub_statuses = list(stub_statuses) if stub_statuses is not None else []
        self.corrupt_readback_once_addr = corrupt_readback_once_addr
        self.probe_err = probe_err
        self.load_pc_dsts: list[int] = []
        self.synced = False
        self._responses: list[bytes] = []

    def write(self, data: bytes) -> int:
        line = data.decode("ascii").strip()
        self.commands.append(line)
        self._responses.append(self._handle(line))
        return len(data)

    def readline(self) -> bytes:
        if not self._responses:
            return b""
        return self._responses.pop(0)

    def _handle(self, line: str) -> bytes:
        if line == "PING":
            return b"OK BSL_BRIDGE\r\n"
        if line in {"ENTER", "RELEASE"}:
            self.synced = False
            return b"OK\r\n"
        if line == "SYNC":
            if self.synced:
                return b"ERR TIMEOUT\r\n"
            self.synced = True
            return b"OK 90\r\n"
        if line.startswith("XFER "):
            assert self.synced, f"XFER called without preceding SYNC: {line!r}"
            self.synced = False
            parts = line.split()
            frame = bytes.fromhex(parts[1])
            self.xfer_frames.append(frame)
            cmd = frame[1]
            if self.fail_cmd == cmd:
                return b"ERR SHORT A0\r\n"
            if cmd == 0x1E:
                if self.locked or self.bsl_ver < 0x0160:
                    return f"{self.probe_err}\r\n".encode("ascii")
                return f"OK {_make_rx_frame(self.rom_id).hex().upper()}\r\n".encode("ascii")
            if cmd == 0x18:
                # Mass Erase clears Main Flash (0x1100..0xFFFF) only, leaving Info Flash untouched
                for a in range(0x1100, 0x10000):
                    self.flash[a] = 0xFF
                return b"OK 90\r\n"
            if cmd == 0x16:
                addr = frame[4] | (frame[5] << 8)
                param = frame[6] | (frame[7] << 8)
                assert param == 0xA502, f"Unexpected Erase Segment param: 0x{param:04X}"
                if 0x1000 <= addr < 0x1100:
                    seg = addr & ~0x7F
                    for a in range(seg, seg + 0x80):
                        self.flash[a] = 0xFF
                elif addr >= 0x1100:
                    seg = addr & ~0x1FF
                    for a in range(seg, min(0x10000, seg + 0x200)):
                        self.flash[a] = 0xFF
                return b"OK 90\r\n"
            if cmd == 0x10:
                pw = frame[8:-2]
                expected_pw = bytes(self.flash[a] for a in range(0xFFE0, 0x10000))
                if pw != expected_pw:
                    return b"ERR SHORT A0\r\n"
                self.locked = False
                return b"OK 90\r\n"
            if cmd == 0x12:
                addr = frame[4] | (frame[5] << 8)
                data = frame[8:-2]
                if 0x0200 <= addr < 0x0A00:
                    for i, b in enumerate(data):
                        self.ram[addr + i] = b
                elif self.bsl_ver < 0x0160 and addr >= 0x8000:
                    # Simulate TI BSL v1.10 signed-address bug ignoring >= 0x8000 writes
                    pass
                else:
                    for i, b in enumerate(data):
                        self.flash[addr + i] &= b
                return b"OK 90\r\n"
            if cmd == 0x14:
                if self.locked:
                    return b"ERR SHORT A0\r\n"
                addr = frame[4] | (frame[5] << 8)
                length = frame[6] | (frame[7] << 8)
                if addr == 0x0FF0 and length == 16:
                    data = self.rom_id
                elif 0x0200 <= addr < 0x0A00:
                    data = bytes(self.ram[addr + i] for i in range(length))
                else:
                    data = bytes(self.flash[addr + i] for i in range(length))
                    if self.corrupt_readback_once_addr == addr:
                        self.corrupt_readback_once_addr = None
                        data = bytes([data[0] ^ 0xFF]) + data[1:]
                return f"OK {_make_rx_frame(data).hex().upper()}\r\n".encode("ascii")
            if cmd == 0x1A:
                addr = frame[4] | (frame[5] << 8)
                assert addr == 0x0300
                stub_len = len(bsl_flash.BSL_V110_RAM_STUB)
                loaded_stub = bytes(self.ram[0x0300 + i] for i in range(stub_len))
                assert loaded_stub == bsl_flash.BSL_V110_RAM_STUB
                dst = self.ram[0x03F0] | (self.ram[0x03F1] << 8)
                words = self.ram[0x03F2] | (self.ram[0x03F3] << 8)
                self.load_pc_dsts.append(dst)
                for i in range(words * 2):
                    self.flash[dst + i] &= self.ram[0x0400 + i]
                st = self.stub_statuses.pop(0) if self.stub_statuses else self.stub_status
                self.ram[0x03F4] = st & 0xFF
                self.ram[0x03F5] = (st >> 8) & 0xFF
                self.locked = True
                return b"OK 90\r\n"
        raise AssertionError(f"Unexpected bridge command: {line!r}")


@pytest.mark.parametrize(
    ("probe_err", "locked"),
    [
        ("ERR SHORT A0", True),
        ("ERR SHORT 70", False),
    ],
)
def test_probe_locked_or_v110_returns_none_and_releases(monkeypatch, probe_err, locked):
    monkeypatch.setattr(bsl_flash.time, "sleep", lambda _s: None)
    fake = FakeBridgeSerial(locked=locked, bsl_ver=0x0110, probe_err=probe_err)
    bridge = bsl_flash.BslBridge(fake)

    chip_id, bsl_ver, payload = bsl_flash.run_probe(bridge)
    assert chip_id is None
    assert bsl_ver is None
    assert payload == b""

    expected_xfer = f"XFER {bsl_flash.build_frame(0x1E, 0, 0).hex().upper()} 22 500"
    assert fake.commands == ["PING", "ENTER", "SYNC", expected_xfer, "RELEASE"]

    cmds_sent = [frame[1] for frame in fake.xfer_frames]
    assert cmds_sent == [0x1E]
    assert 0x18 not in cmds_sent
    assert 0x10 not in cmds_sent


def test_probe_unlocked_reads_version_and_releases(monkeypatch):
    monkeypatch.setattr(bsl_flash.time, "sleep", lambda _s: None)
    fake = FakeBridgeSerial(locked=False, bsl_ver=0x0160)
    bridge = bsl_flash.BslBridge(fake)

    chip_id, bsl_ver, payload = bsl_flash.run_probe(bridge)
    assert chip_id == 0xF149
    assert bsl_ver == 0x0160
    assert len(payload) == 16

    expected_xfer = f"XFER {bsl_flash.build_frame(0x1E, 0, 0).hex().upper()} 22 500"
    assert fake.commands == ["PING", "ENTER", "SYNC", expected_xfer, "RELEASE"]

    cmds_sent = [frame[1] for frame in fake.xfer_frames]
    assert cmds_sent == [0x1E]
    assert 0x18 not in cmds_sent
    assert 0x10 not in cmds_sent


def test_flash_sequence_sync_and_verify(monkeypatch):
    monkeypatch.setattr(bsl_flash.time, "sleep", lambda _s: None)
    fake = FakeBridgeSerial(locked=True, bsl_ver=0x0160)
    bridge = bsl_flash.BslBridge(fake)
    chunks = [
        (0x1100, b"\x12\x34\x56\x78"),
        (0xFFFE, b"\x00\x11"),
    ]

    chip_id, bsl_ver, _ = bsl_flash.run_flash(bridge, chunks, verify=True)
    assert chip_id == 0xF149
    assert bsl_ver == 0x0160

    # Every XFER must be immediately preceded by SYNC and no two SYNCs may be consecutive
    for idx, cmd_line in enumerate(fake.commands):
        if cmd_line.startswith("XFER "):
            assert idx > 0 and fake.commands[idx - 1] == "SYNC"
        if cmd_line == "SYNC":
            assert idx + 1 < len(fake.commands) and fake.commands[idx + 1].startswith("XFER ")

    cmds_sent = [frame[1] for frame in fake.xfer_frames]
    assert cmds_sent == [0x18, 0x10, 0x14, 0x12, 0x12, 0x14, 0x14, 0x14]
    # Last TX Data Block checks interrupt vector table 0xFFE0..0xFFFF (32 bytes)
    last_tx_frame = fake.xfer_frames[-1]
    assert (last_tx_frame[4] | (last_tx_frame[5] << 8)) == 0xFFE0
    assert (last_tx_frame[6] | (last_tx_frame[7] << 8)) == 32
    assert fake.commands[-1] == "RELEASE"


def test_flash_v110_ram_stub_and_vector_last_order(monkeypatch):
    monkeypatch.setattr(bsl_flash.time, "sleep", lambda _s: None)
    fake = FakeBridgeSerial(locked=True, bsl_ver=0x0110)
    bridge = bsl_flash.BslBridge(fake)
    # Deliberately place the reset vector chunk first to verify vector-last reordering
    chunks = [
        (0xFFFE, b"\x00\x11"),
        (0x1100, b"\x12\x34"),
        (0x8000, b"\xaa\xbb"),
    ]

    chip_id, bsl_ver, _ = bsl_flash.run_flash(bridge, chunks, verify=True)
    assert chip_id == 0xF149
    assert bsl_ver == 0x0110

    assert fake.load_pc_dsts == [0x1100, 0x8000, 0xFFFE]
    assert fake.flash[0x1100] == 0x12
    assert fake.flash[0x1101] == 0x34
    assert fake.flash[0x8000] == 0xAA
    assert fake.flash[0x8001] == 0xBB
    assert fake.flash[0xFFFE] == 0x00
    assert fake.flash[0xFFFF] == 0x11
    assert (fake.ram[0x03F4] | (fake.ram[0x03F5] << 8)) == 0xCAFE
    assert fake.commands[-1] == "RELEASE"


def test_flash_v110_raises_if_stub_status_not_cafe(monkeypatch):
    monkeypatch.setattr(bsl_flash.time, "sleep", lambda _s: None)
    fake = FakeBridgeSerial(locked=True, bsl_ver=0x0110, stub_status=0xDEAD)
    bridge = bsl_flash.BslBridge(fake)
    chunks = [(0x1100, b"\x12\x34"), (0xFFFE, b"\x00\x11")]

    with pytest.raises(bsl_flash.BslError, match="status=0xDEAD"):
        bsl_flash.run_flash(bridge, chunks, verify=True)

    assert fake.load_pc_dsts == [0x1100, 0x1100, 0x1100]
    assert fake.commands[-1] == "RELEASE"


def test_flash_v110_retries_transient_stub_status_and_readback_mismatch(monkeypatch):
    sleeps: list[float] = []
    monkeypatch.setattr(bsl_flash.time, "sleep", lambda s: sleeps.append(s))
    fake = FakeBridgeSerial(
        locked=True,
        bsl_ver=0x0110,
        stub_statuses=[0x0000, 0xCAFE, 0xCAFE, 0xCAFE],
        corrupt_readback_once_addr=0x8000,
    )
    bridge = bsl_flash.BslBridge(fake)
    chunks = [
        (0x1100, b"\x12\x34"),
        (0x8000, b"\xaa\xbb"),
        (0xFFFE, b"\x00\x11"),
    ]

    chip_id, bsl_ver, _ = bsl_flash.run_flash(bridge, chunks, verify=True)
    assert chip_id == 0xF149
    assert bsl_ver == 0x0110

    # 0x1100 retried once on status=0x0000; 0x8000 retried once on readback mismatch; 0xFFFE once
    assert fake.load_pc_dsts == [0x1100, 0x1100, 0x8000, 0x8000, 0xFFFE]
    stub_uploads = [
        f
        for f in fake.xfer_frames
        if f[1] == 0x12 and (f[4] | (f[5] << 8)) == 0x0300 and f[8:-2] == bsl_flash.BSL_V110_RAM_STUB
    ]
    assert len(stub_uploads) == 3  # initial upload + 2 retries
    assert any(s >= 0.05 for s in sleeps)
    assert fake.flash[0x1100] == 0x12
    assert fake.flash[0x8000] == 0xAA
    assert fake.flash[0xFFFE] == 0x00
    assert fake.commands[-1] == "RELEASE"


def test_flash_allow_info_erases_touched_segments(monkeypatch):
    monkeypatch.setattr(bsl_flash.time, "sleep", lambda _s: None)
    fake = FakeBridgeSerial(info_fill=0x00, locked=True, bsl_ver=0x0160)
    bridge = bsl_flash.BslBridge(fake)
    mem = {
        0x1000: 0xAA,
        0x1001: 0x55,
        0x1100: 0x12,
        0x1101: 0x34,
        0xFFFE: 0x00,
        0xFFFF: 0x11,
    }
    chunks = bsl_flash.prepare_flash_chunks(mem, allow_info=True)

    bsl_flash.run_flash(bridge, chunks, verify=True)

    cmds_sent = [frame[1] for frame in fake.xfer_frames]
    # Mass Erase -> RX Password -> ROM ID (0x14) -> Erase Segment (0x1000) -> 3x RX -> 4x TX
    assert cmds_sent == [0x18, 0x10, 0x14, 0x16, 0x12, 0x12, 0x12, 0x14, 0x14, 0x14, 0x14]
    erase_seg_frame = fake.xfer_frames[3]
    assert (erase_seg_frame[4] | (erase_seg_frame[5] << 8)) == 0x1000
    assert fake.flash[0x1000] == 0xAA
    assert fake.flash[0x1001] == 0x55
    # Untouched Info A (0x1080..0x10FF) stays 0x00
    assert fake.flash[0x1080] == 0x00
    assert fake.commands[-1] == "RELEASE"


def test_flash_releases_on_mid_flash_error(monkeypatch):
    monkeypatch.setattr(bsl_flash.time, "sleep", lambda _s: None)
    fake = FakeBridgeSerial(fail_cmd=0x12)
    bridge = bsl_flash.BslBridge(fake)
    chunks = [(0x1100, b"\x12\x34"), (0xFFFE, b"\x00\x11")]

    with pytest.raises(bsl_flash.BslError, match="ERR SHORT A0"):
        bsl_flash.run_flash(bridge, chunks, verify=True)

    assert fake.commands[-1] == "RELEASE"


def test_cli_flash_requires_confirm_erase_without_serial(tmp_path, monkeypatch, capsys):
    hex_file = tmp_path / "app.hex"
    hex_file.write_text(":02FFFE000011F0\n:00000001FF\n", encoding="utf-8")

    def _fail(*_args, **_kwargs):
        raise AssertionError("Serial must not be accessed without --confirm-erase")

    monkeypatch.setattr(bsl_flash, "_select_port", _fail)
    monkeypatch.setattr(bsl_flash.serial, "Serial", _fail)

    rc = bsl_flash.main(["flash", str(hex_file)])
    assert rc != 0
    err = capsys.readouterr().err
    assert "--confirm-erase" in err
    assert "Mass Erase" in err


def test_cli_probe_locked_prints_status(monkeypatch, capsys):
    monkeypatch.setattr(bsl_flash.time, "sleep", lambda _s: None)
    fake = FakeBridgeSerial(locked=True)

    class _FakePort:
        def __enter__(self):
            return self

        def __exit__(self, *_exc):
            return False

        def reset_input_buffer(self):
            pass

        def write(self, data: bytes) -> int:
            return fake.write(data)

        def readline(self) -> bytes:
            return fake.readline()

    fake_port = _FakePort()
    monkeypatch.setattr(bsl_flash, "_select_port", lambda _p: "COM99")
    monkeypatch.setattr(bsl_flash.serial, "Serial", lambda *_a, **_kw: fake_port)

    rc = bsl_flash.main(["probe", "--port", "COM99"])
    assert rc == 0
    out = capsys.readouterr().out
    assert "BSL SYNC OK (0x90)" in out
    assert "device locked" in out
    assert fake.commands[-1] == "RELEASE"

