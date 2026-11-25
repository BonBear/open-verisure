#!/usr/bin/env python3
"""bsl_flash.py - Host tool for the ESP32-C3 TI SLAU319 ROM BSL bridge (MSP430F149).

Supports reading chip/BSL version (`probe`, 100% read-only) and erasing,
programming, password-unlocking, and verifying Intel HEX or TI-TXT images
(`flash --confirm-erase`).
"""

from __future__ import annotations

import argparse
import struct
import sys
import time
from pathlib import Path
from typing import Protocol

import serial

# 80-byte position-independent MSP430 RAM flash-writer stub (loaded at 0x0300) for BSL v1.10:
#   SP = 0x09FE, WDTCTL = 0x5A80 (stop WDT), FCTL2 = 0xA542 (MCLK/3),
#   FCTL3 = 0xA500 (unlock flash), FCTL1 = 0xA540 (WRT).
#   Copies *(uint16_t *)0x03F2 words from 0x0400 to *(uint16_t *)0x03F0,
#   polling (FCTL3 & BUSY) after each word write; locks flash (FCTL1 = 0xA500,
#   FCTL3 = 0xA510), writes 0xCAFE to 0x03F4, and loops forever (jmp $).
BSL_V110_RAM_STUB = bytes.fromhex(
    "3140FE09B240805A2001B24042A52A01B24000A52C01B24040A52801"
    "1C42F0031D42F2033E4000040D930824BC4E000092B32C01FD232C53"
    "1D83F63FB24000A52801B24010A52C01B240FECAF403FF3F"
)


class BslError(RuntimeError):
    """Raised when the ESP32 BSL bridge or MSP430 ROM BSL reports an error."""


class SerialLike(Protocol):
    """Minimal serial-port interface used by BslBridge."""

    def write(self, data: bytes) -> int | None: ...
    def readline(self) -> bytes: ...


def bsl_checksum(data: bytes) -> tuple[int, int]:
    """Return (CKL, CKH) per TI SLAU319 §2.3 (bitwise-inverted even/odd XOR)."""
    if len(data) < 2 or len(data) % 2 != 0:
        raise ValueError(f"BSL checksum requires an even length >= 2, got {len(data)}")
    ckl = 0xFF
    ckh = 0xFF
    for i in range(0, len(data), 2):
        ckl ^= data[i]
        ckh ^= data[i + 1]
    return ckl, ckh


def build_frame(cmd: int, addr: int, param: int, payload: bytes = b"") -> bytes:
    """Build a TI SLAU319 BSL request frame: 80 CMD L1 L2 AL AH LL LH [payload] CKL CKH."""
    if not (0 <= cmd <= 0xFF):
        raise ValueError(f"Invalid BSL command byte: 0x{cmd:X}")
    if not (0 <= addr <= 0xFFFF):
        raise ValueError(f"Invalid 16-bit address: 0x{addr:X}")
    if not (0 <= param <= 0xFFFF):
        raise ValueError(f"Invalid 16-bit parameter: 0x{param:X}")
    if len(payload) % 2 != 0 or len(payload) > 240:
        raise ValueError(f"Payload length must be even and <= 240, got {len(payload)}")

    length = 4 + len(payload)
    body = bytes([
        0x80,
        cmd,
        length,
        length,
        addr & 0xFF,
        (addr >> 8) & 0xFF,
        param & 0xFF,
        (param >> 8) & 0xFF,
    ]) + payload
    ckl, ckh = bsl_checksum(body)
    return body + bytes([ckl, ckh])


def verify_rx_frame(raw: bytes, expected_len: int) -> bytes:
    """Validate a BSL data response frame and return its payload."""
    if expected_len < 0 or expected_len > 250 or expected_len % 2 != 0:
        raise ValueError(f"Invalid expected payload length: {expected_len}")
    if len(raw) != expected_len + 6:
        raise ValueError(f"Expected {expected_len + 6} RX bytes, got {len(raw)}")
    if raw[0] != 0x80 or raw[1] != 0x00:
        raise ValueError(f"Invalid BSL RX header: {raw[:2].hex().upper()}")
    if raw[2] != expected_len or raw[3] != expected_len:
        raise ValueError(
            f"Length mismatch in RX frame: L1={raw[2]} L2={raw[3]} expected={expected_len}"
        )
    exp_ckl, exp_ckh = bsl_checksum(raw[:-2])
    if raw[-2] != exp_ckl or raw[-1] != exp_ckh:
        raise ValueError(
            f"BSL RX checksum mismatch: got {raw[-2]:02X}{raw[-1]:02X}, "
            f"expected {exp_ckl:02X}{exp_ckh:02X}"
        )
    return bytes(raw[4:-2])


def parse_ihex(text: str) -> dict[int, int]:
    """Parse an Intel HEX string into a byte map {address: byte}."""
    mem: dict[int, int] = {}
    base_addr = 0
    saw_eof = False

    for line_no, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line:
            continue
        if saw_eof:
            raise ValueError(f"Intel HEX line {line_no}: unexpected record after EOF")
        if not line.startswith(":") or len(line) < 11 or (len(line) - 1) % 2 != 0:
            raise ValueError(f"Intel HEX line {line_no}: malformed record")
        try:
            record = bytes.fromhex(line[1:])
        except ValueError as exc:
            raise ValueError(f"Intel HEX line {line_no}: invalid hex characters") from exc

        if (sum(record) & 0xFF) != 0:
            raise ValueError(f"Intel HEX line {line_no}: bad checksum")

        byte_count = record[0]
        offset = (record[1] << 8) | record[2]
        rec_type = record[3]
        data = record[4:-1]
        if len(data) != byte_count:
            raise ValueError(
                f"Intel HEX line {line_no}: byte count {byte_count} != payload len {len(data)}"
            )

        if rec_type == 0x00:
            for i, b in enumerate(data):
                mem[base_addr + offset + i] = b
        elif rec_type == 0x01:
            if byte_count != 0:
                raise ValueError(f"Intel HEX line {line_no}: EOF record must have 0 bytes")
            saw_eof = True
        elif rec_type == 0x02:
            if byte_count != 2:
                raise ValueError(f"Intel HEX line {line_no}: type 02 must have 2 bytes")
            base_addr = ((data[0] << 8) | data[1]) << 4
        elif rec_type == 0x03:
            if byte_count != 4:
                raise ValueError(f"Intel HEX line {line_no}: type 03 must have 4 bytes")
        elif rec_type == 0x04:
            if byte_count != 2:
                raise ValueError(f"Intel HEX line {line_no}: type 04 must have 2 bytes")
            base_addr = ((data[0] << 8) | data[1]) << 16
        elif rec_type == 0x05:
            if byte_count != 4:
                raise ValueError(f"Intel HEX line {line_no}: type 05 must have 4 bytes")
        else:
            raise ValueError(f"Intel HEX line {line_no}: unsupported record type 0x{rec_type:02X}")

    if not saw_eof:
        raise ValueError("Intel HEX missing EOF (:00000001FF) record")
    return mem


def parse_titxt(text: str) -> dict[int, int]:
    """Parse a TI-TXT string into a byte map {address: byte}."""
    mem: dict[int, int] = {}
    addr: int | None = None
    saw_q = False

    for line_no, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line:
            continue
        if saw_q:
            raise ValueError(f"TI-TXT line {line_no}: unexpected content after 'q'")
        if line.lower() == "q":
            saw_q = True
            continue
        if line.startswith("@"):
            addr_str = line[1:].strip()
            if not addr_str:
                raise ValueError(f"TI-TXT line {line_no}: missing address after '@'")
            try:
                addr = int(addr_str, 16)
            except ValueError as exc:
                raise ValueError(f"TI-TXT line {line_no}: invalid section address {line!r}") from exc
            continue
        if addr is None:
            raise ValueError(f"TI-TXT line {line_no}: data before '@ADDR' section")

        for tok in line.split():
            if tok.lower() == "q":
                saw_q = True
                break
            if len(tok) != 2:
                raise ValueError(f"TI-TXT line {line_no}: invalid byte token {tok!r}")
            try:
                val = int(tok, 16)
            except ValueError as exc:
                raise ValueError(f"TI-TXT line {line_no}: invalid hex byte {tok!r}") from exc
            mem[addr] = val
            addr += 1

    if not saw_q:
        raise ValueError("TI-TXT missing 'q' terminator")
    return mem


def load_image(path: Path) -> dict[int, int]:
    """Load an Intel HEX or TI-TXT image file based on extension or content."""
    text = path.read_text(encoding="utf-8")
    suffix = path.suffix.lower()
    if suffix in {".hex", ".ihex", ".ihx"}:
        return parse_ihex(text)
    if suffix == ".txt":
        return parse_titxt(text)

    for line in text.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        if stripped.startswith(":"):
            return parse_ihex(text)
        if stripped.startswith("@"):
            return parse_titxt(text)
        break
    raise ValueError(f"Cannot detect image format for {path}")


def prepare_flash_chunks(
    mem: dict[int, int],
    allow_info: bool = False,
    max_chunk: int = 96,
    allow_no_reset_vec: bool = False,
) -> list[tuple[int, bytes]]:
    """Validate addresses, align spans to even boundaries, and split into BSL chunks."""
    if max_chunk < 2 or max_chunk > 240 or max_chunk % 2 != 0:
        raise ValueError(f"max_chunk must be even and in 2..240, got {max_chunk}")
    if not mem:
        raise ValueError("Image contains no bytes")

    for addr, val in mem.items():
        if not (0 <= val <= 0xFF):
            raise ValueError(f"Invalid byte value {val} at 0x{addr:04X}")
        if addr < 0x1000 or addr > 0xFFFF:
            raise ValueError(
                f"Address 0x{addr:04X} is outside MSP430F149 flash (BSL/RAM/SFR forbidden)"
            )
        if addr < 0x1100 and not allow_info:
            raise ValueError(
                f"Address 0x{addr:04X} is in Info Flash (0x1000..0x10FF); pass allow_info=True to write"
            )

    if not allow_no_reset_vec:
        if 0xFFFE not in mem or 0xFFFF not in mem:
            raise ValueError("Reset vector 0xFFFE..0xFFFF is missing from image")
        reset_vec = mem[0xFFFE] | (mem[0xFFFF] << 8)
        if reset_vec == 0xFFFF:
            raise ValueError("Reset vector 0xFFFE..0xFFFF is 0xFFFF (blank)")

    sorted_addrs = sorted(mem.keys())
    spans: list[tuple[int, int]] = []
    span_start = sorted_addrs[0]
    span_end = sorted_addrs[0]
    for addr in sorted_addrs[1:]:
        if addr == span_end + 1:
            span_end = addr
        else:
            spans.append((span_start, span_end))
            span_start = addr
            span_end = addr
    spans.append((span_start, span_end))

    chunks: list[tuple[int, bytes]] = []
    for raw_start, raw_end in spans:
        aligned_start = raw_start & ~1
        aligned_end = raw_end | 1
        span_bytes = bytes(mem.get(a, 0xFF) for a in range(aligned_start, aligned_end + 1))
        for offset in range(0, len(span_bytes), max_chunk):
            piece = span_bytes[offset : offset + max_chunk]
            if all(b == 0xFF for b in piece):
                continue
            chunks.append((aligned_start + offset, piece))

    return chunks


class BslBridge:
    """Host client for the ESP32-C3 UART-to-MSP430-BSL bridge protocol."""

    def __init__(self, ser: SerialLike) -> None:
        self.ser = ser

    def _send_cmd(self, cmd: str) -> str:
        self.ser.write((cmd + "\n").encode("ascii"))
        if hasattr(self.ser, "flush"):
            self.ser.flush()
        while True:
            raw = self.ser.readline()
            if not raw:
                raise BslError(f"Timeout waiting for bridge response to {cmd.split()[0]}")
            line = raw.decode("ascii", errors="replace").strip()
            if not line:
                continue
            if line.startswith("ERR "):
                raise BslError(f"{cmd.split()[0]} failed: {line}")
            if line == "OK" or line.startswith("OK "):
                return line
            raise BslError(f"Unexpected bridge response to {cmd.split()[0]}: {line!r}")

    def ping(self) -> None:
        resp = self._send_cmd("PING")
        if resp != "OK BSL_BRIDGE":
            raise BslError(f"Unexpected PING response: {resp!r}")

    def enter(self) -> None:
        resp = self._send_cmd("ENTER")
        if resp != "OK":
            raise BslError(f"Unexpected ENTER response: {resp!r}")

    def sync(self) -> None:
        resp = self._send_cmd("SYNC")
        if resp != "OK 90":
            raise BslError(f"Unexpected SYNC response: {resp!r}")

    def xfer(self, tx: bytes, expect_rx: int, timeout_ms: int = 500) -> bytes:
        if not (1 <= len(tx) <= 255):
            raise ValueError(f"TX length must be 1..255, got {len(tx)}")
        if not (0 <= expect_rx <= 255):
            raise ValueError(f"expect_rx must be 0..255, got {expect_rx}")
        if not (1 <= timeout_ms <= 10000):
            raise ValueError(f"timeout_ms must be 1..10000, got {timeout_ms}")

        resp = self._send_cmd(f"XFER {tx.hex().upper()} {expect_rx} {timeout_ms}")
        if expect_rx == 0:
            if resp != "OK":
                raise BslError(f"Unexpected XFER response: {resp!r}")
            return b""
        if not resp.startswith("OK "):
            raise BslError(f"Expected hex payload in XFER response, got {resp!r}")
        rx = bytes.fromhex(resp[3:])
        if len(rx) != expect_rx:
            raise BslError(f"Expected {expect_rx} bytes from XFER, got {len(rx)}")
        return rx

    def release(self) -> None:
        resp = self._send_cmd("RELEASE")
        if resp != "OK":
            raise BslError(f"Unexpected RELEASE response: {resp!r}")

    def tx_bsl_version(self) -> tuple[int, int, bytes]:
        time.sleep(0.005)
        self.sync()
        raw = self.xfer(build_frame(0x1E, 0x0000, 0x0000), 22, timeout_ms=500)
        payload = verify_rx_frame(raw, 16)
        chip_id = (payload[0] << 8) | payload[1]
        bsl_version = (payload[10] << 8) | payload[11]
        return chip_id, bsl_version, payload

    def mass_erase(self) -> None:
        time.sleep(0.005)
        self.sync()
        raw = self.xfer(build_frame(0x18, 0xFF00, 0xA506), 1, timeout_ms=500)
        if raw != b"\x90":
            raise BslError(f"Mass Erase NAK: {raw.hex().upper()}")
        time.sleep(0.025)

    def rx_password(self, pw: bytes = b"\xff" * 32) -> None:
        if len(pw) != 32:
            raise ValueError(f"BSL password must be 32 bytes, got {len(pw)}")
        time.sleep(0.005)
        self.sync()
        raw = self.xfer(build_frame(0x10, 0x0000, 0x0000, pw), 1, timeout_ms=500)
        if raw != b"\x90":
            raise BslError(f"RX Password NAK: {raw.hex().upper()}")

    def erase_segment(self, addr: int) -> None:
        if not (0 <= addr <= 0xFFFF):
            raise ValueError(f"Invalid 16-bit address: 0x{addr:X}")
        time.sleep(0.005)
        self.sync()
        raw = self.xfer(build_frame(0x16, addr, 0xA502), 1, timeout_ms=500)
        if raw != b"\x90":
            raise BslError(f"Erase Segment NAK at 0x{addr:04X}: {raw.hex().upper()}")
        time.sleep(0.025)

    def rx_data_block(self, addr: int, data: bytes) -> None:
        if not data or len(data) % 2 != 0 or len(data) > 240:
            raise ValueError(f"Data block length must be even and 2..240, got {len(data)}")
        time.sleep(0.005)
        self.sync()
        raw = self.xfer(build_frame(0x12, addr, len(data), data), 1, timeout_ms=1000)
        if raw != b"\x90":
            raise BslError(f"RX Data Block NAK at 0x{addr:04X}: {raw.hex().upper()}")

    def tx_data_block(self, addr: int, length: int) -> bytes:
        if length <= 0 or length % 2 != 0 or length > 240:
            raise ValueError(f"Read length must be even and 2..240, got {length}")
        time.sleep(0.005)
        self.sync()
        raw = self.xfer(build_frame(0x14, addr, length), length + 6, timeout_ms=1000)
        return verify_rx_frame(raw, length)

    def load_pc(self, addr: int) -> None:
        if not (0 <= addr <= 0xFFFF):
            raise ValueError(f"Invalid 16-bit address: 0x{addr:X}")
        time.sleep(0.005)
        self.sync()
        raw = self.xfer(build_frame(0x1A, addr, 0x0000), 1, timeout_ms=500)
        if raw != b"\x90":
            raise BslError(f"Load PC NAK at 0x{addr:04X}: {raw.hex().upper()}")

    def read_bsl_rom_id(self) -> tuple[int, int, bytes]:
        payload = self.tx_data_block(0x0FF0, 16)
        chip_id = (payload[0] << 8) | payload[1]
        bsl_ver = (payload[10] << 8) | payload[11]
        return chip_id, bsl_ver, payload


def run_probe(bridge: BslBridge) -> tuple[int | None, int | None, bytes]:
    """Read chip ID and BSL version without erasing or sending a password."""
    bridge.ping()
    bridge.enter()
    try:
        try:
            return bridge.tx_bsl_version()
        except BslError as exc:
            msg = str(exc)
            if "ERR SHORT A0" in msg or "ERR SHORT 70" in msg:
                return None, None, b""
            raise
    finally:
        bridge.release()


def _touches_vectors(chunk: tuple[int, bytes]) -> bool:
    addr, data = chunk
    return (addr + len(data)) > 0xFFE0 and addr <= 0xFFFF


def run_flash(
    bridge: BslBridge,
    chunks: list[tuple[int, bytes]],
    verify: bool = True,
) -> tuple[int, int, bytes]:
    """Mass-erase, unlock, read BSL version, program (RAM stub for v1.10), and verify."""
    bridge.ping()
    bridge.enter()
    try:
        bridge.mass_erase()
        bridge.rx_password()
        chip_id, bsl_ver, payload = bridge.read_bsl_rom_id()

        has_info_b = any(addr < 0x1080 and (addr + len(data)) > 0x1000 for addr, data in chunks)
        has_info_a = any(addr < 0x1100 and (addr + len(data)) > 0x1080 for addr, data in chunks)
        if has_info_b:
            bridge.erase_segment(0x1000)
        if has_info_a:
            bridge.erase_segment(0x1080)

        ordered_chunks = [c for c in chunks if not _touches_vectors(c)] + [
            c for c in chunks if _touches_vectors(c)
        ]

        written_bytes: dict[int, int] = {}
        for addr, data in ordered_chunks:
            for i, b in enumerate(data):
                written_bytes[addr + i] = b
        final_pw = bytes(written_bytes.get(a, 0xFF) for a in range(0xFFE0, 0x10000))

        if bsl_ver < 0x0160:
            if ordered_chunks:
                bridge.rx_data_block(0x0300, BSL_V110_RAM_STUB)
            cur_pw_map = {a: 0xFF for a in range(0xFFE0, 0x10000)}
            for addr, data in ordered_chunks:
                hdr = struct.pack("<HHH", addr, len(data) // 2, 0x0000)
                last_err: BslError | None = None
                for attempt in range(3):
                    if attempt > 0 or bridge.tx_data_block(0x0300, 2) != b"\x31\x40":
                        bridge.rx_data_block(0x0300, BSL_V110_RAM_STUB)
                    bridge.rx_data_block(0x03F0, hdr)
                    bridge.rx_data_block(0x0400, data)
                    bridge.load_pc(0x0300)
                    time.sleep(max(0.05, len(data) * 0.00015))
                    cand_pw_map = dict(cur_pw_map)
                    for i, b in enumerate(data):
                        if 0xFFE0 <= addr + i <= 0xFFFF:
                            cand_pw_map[addr + i] = b
                    next_pw = bytes(cand_pw_map[a] for a in range(0xFFE0, 0x10000))
                    bridge.enter()
                    bridge.rx_password(next_pw)
                    if verify:
                        readback = bridge.tx_data_block(addr, len(data))
                        if readback != data:
                            status_bytes = bridge.tx_data_block(0x03F4, 2)
                            (status,) = struct.unpack("<H", status_bytes)
                            if status != 0xCAFE:
                                last_err = BslError(
                                    f"BSL v1.10 RAM stub failed at 0x{addr:04X}: status=0x{status:04X}"
                                )
                            else:
                                last_err = BslError(f"Verification mismatch at 0x{addr:04X}")
                            continue
                    else:
                        status_bytes = bridge.tx_data_block(0x03F4, 2)
                        (status,) = struct.unpack("<H", status_bytes)
                        if status != 0xCAFE:
                            last_err = BslError(
                                f"BSL v1.10 RAM stub failed at 0x{addr:04X}: status=0x{status:04X}"
                            )
                            continue
                    cur_pw_map = cand_pw_map
                    break
                else:
                    assert last_err is not None
                    raise last_err
        else:
            for addr, data in ordered_chunks:
                bridge.rx_data_block(addr, data)

        if verify:
            for addr, data in ordered_chunks:
                readback = bridge.tx_data_block(addr, len(data))
                if readback != data:
                    raise BslError(f"Verification mismatch at 0x{addr:04X}")

            actual_vectors = bridge.tx_data_block(0xFFE0, 32)
            if actual_vectors != final_pw:
                raise BslError("Verification mismatch in interrupt vector table 0xFFE0..0xFFFF")

        return chip_id, bsl_ver, payload
    finally:
        bridge.release()


def _select_port(preferred_port: str | None) -> str | None:
    repo_root = str(Path(__file__).resolve().parents[2])
    if repo_root not in sys.path:
        sys.path.insert(0, repo_root)
    from port_selector import select_com_port

    return select_com_port(
        preferred_port=preferred_port,
        hint_keyword="ESP32",
        prompt_title="Select ESP32-C3 BSL Bridge Port",
    )


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="ESP32-C3 TI SLAU319 ROM BSL flasher for MSP430F149",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    probe_p = sub.add_parser(
        "probe",
        help="Read-only check: PING -> ENTER -> SYNC -> TX BSL Version -> RELEASE",
    )
    probe_p.add_argument("--port", help="Serial port of the ESP32-C3 BSL bridge")

    flash_p = sub.add_parser(
        "flash",
        help="Mass-erase, program, and verify an Intel HEX or TI-TXT image",
    )
    flash_p.add_argument("image", type=Path, help="Path to Intel HEX (.hex) or TI-TXT (.txt)")
    flash_p.add_argument("--port", help="Serial port of the ESP32-C3 BSL bridge")
    flash_p.add_argument(
        "--confirm-erase",
        action="store_true",
        help="Required confirmation: Mass Erase permanently erases all factory MSP430 flash",
    )
    flash_p.add_argument(
        "--no-verify",
        action="store_true",
        help="Skip TX Data Block read-back verification after programming",
    )
    flash_p.add_argument(
        "--allow-info",
        action="store_true",
        help="Allow writing Info Flash (0x1000..0x10FF)",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)

    if args.command == "flash" and not args.confirm_erase:
        print(
            "ERROR: Refusing to flash without --confirm-erase.\n"
            "WARNING: MSP430 ROM BSL flashing runs Mass Erase first, which PERMANENTLY\n"
            "erases all factory firmware and data on the target MSP430F149!",
            file=sys.stderr,
        )
        return 2

    chunks: list[tuple[int, bytes]] = []
    if args.command == "flash":
        mem = load_image(args.image)
        chunks = prepare_flash_chunks(mem, allow_info=args.allow_info)
        total_bytes = sum(len(data) for _, data in chunks)
        print(f"Prepared {len(chunks)} chunk(s), {total_bytes} byte(s) from {args.image}")

    port = _select_port(args.port)
    if not port:
        print("ERROR: No serial port selected.", file=sys.stderr)
        return 1

    with serial.Serial(port, 115200, timeout=2.0) as ser:
        ser.reset_input_buffer()
        bridge = BslBridge(ser)
        if args.command == "probe":
            chip_id, bsl_ver, payload = run_probe(bridge)
            if chip_id is None or bsl_ver is None:
                print(
                    "BSL SYNC OK (0x90) — device locked or BSL v1.10 "
                    "(TX BSL Version requires unlock / 0x14 read)"
                )
            else:
                print(
                    f"Chip ID: 0x{chip_id:04X} | BSL Version: 0x{bsl_ver:04X} "
                    f"(v{bsl_ver >> 8}.{bsl_ver & 0xFF:02X})"
                )
                print(f"Raw BSL info (16 B): {payload.hex().upper()}")
        elif args.command == "flash":
            chip_id, bsl_ver, _ = run_flash(bridge, chunks, verify=not args.no_verify)
            print(
                f"Chip ID: 0x{chip_id:04X} | BSL Version: 0x{bsl_ver:04X} "
                f"(v{bsl_ver >> 8}.{bsl_ver & 0xFF:02X})"
            )
            print("Flash complete" + (" and verified." if not args.no_verify else "."))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

