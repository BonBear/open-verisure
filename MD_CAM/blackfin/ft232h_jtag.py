#!/usr/bin/env python3
"""
ft232h_jtag.py - Minimal JTAG over a FT232H (MPSSE) through WinUSB/libusb, without libftdi.

The ADI toolchain's bfin-jtag/bfin-gdbproxy (2014) bundle an old libusb that cannot enumerate
the FT232H on Windows 11, so this talks to the chip directly with pyusb + libusb-package.

Wiring FT232H -> J5: AD0=TCK (5), AD1=TDI (3), AD2=TDO (7), AD3=TMS (4), GND (6).

Usage:
    python ft232h_jtag.py idcode          scan the chain and print every IDCODE
"""
import sys
import time

import libusb_package
import usb.core
import usb.util

VID, PID = 0x0403, 0x6014
EP_OUT, EP_IN = 0x02, 0x81
TCK_HZ = 1_000_000

# FTDI vendor requests
SIO_RESET, SIO_SET_LATENCY, SIO_SET_BITMODE = 0x00, 0x09, 0x0B
BITMODE_RESET, BITMODE_MPSSE = 0x00, 0x02

# Low byte: AD0 TCK out, AD1 TDI out, AD2 TDO in, AD3 TMS out
GPIO_DIR = 0x0B
GPIO_IDLE = 0x08  # TMS high, TCK low


class FT232H:
    def __init__(self, hz: int = TCK_HZ):
        self.dev = libusb_package.find(idVendor=VID, idProduct=PID)
        if self.dev is None:
            sys.exit("[-] FT232H (0403:6014) not found; is the WinUSB driver installed?")
        self.dev.set_configuration()
        self._ctrl(SIO_RESET, 0)
        self._ctrl(SIO_RESET, 1)  # purge RX
        self._ctrl(SIO_RESET, 2)  # purge TX
        self._ctrl(SIO_SET_LATENCY, 2)
        self._ctrl(SIO_SET_BITMODE, BITMODE_RESET << 8)
        self._ctrl(SIO_SET_BITMODE, (BITMODE_MPSSE << 8) | GPIO_DIR)
        time.sleep(0.05)
        self._drain()
        div = max(0, 60_000_000 // (2 * hz) - 1)
        self.write(bytes([
            0x8A,                      # 60 MHz base clock (no divide by 5)
            0x97,                      # no adaptive clocking
            0x8D,                      # no 3-phase clocking
            0x85,                      # loopback off
            0x86, div & 0xFF, div >> 8,
            0x80, GPIO_IDLE, GPIO_DIR,
        ]))

    def _ctrl(self, req: int, value: int) -> None:
        self.dev.ctrl_transfer(0x40, req, value, 1, None, 1000)

    def _drain(self) -> None:
        try:
            while len(self.dev.read(EP_IN, 512, 50)) > 2:
                pass
        except usb.core.USBTimeoutError:
            pass

    def write(self, data: bytes) -> None:
        self.dev.write(EP_OUT, data, 1000)

    def read(self, n: int) -> bytes:
        """Read n payload bytes; every USB packet starts with 2 modem status bytes."""
        out = bytearray()
        t0 = time.time()
        while len(out) < n:
            pkt = self.dev.read(EP_IN, 512, 1000)
            out += bytes(pkt[2:])
            if time.time() - t0 > 2:
                raise TimeoutError(f"MPSSE read: got {len(out)} of {n} bytes")
        return bytes(out[:n])

    # --- JTAG primitives -------------------------------------------------------------------
    def tms(self, bits: str, tdi: int = 1) -> None:
        """Clock TMS bits (string of '0'/'1', first bit first), at most 7 per command."""
        cmd = bytearray()
        for i in range(0, len(bits), 7):
            chunk = bits[i:i + 7]
            val = sum(1 << j for j, b in enumerate(chunk) if b == "1") | (0x80 if tdi else 0)
            cmd += bytes([0x4B, len(chunk) - 1, val])
        self.write(bytes(cmd))

    def shift(self, nbits: int, tdi: int = 0, exit_shift: bool = True) -> int:
        """Shift nbits LSB first in the current Shift-DR/IR state and return what came out
        on TDO. With exit_shift the last bit is clocked with TMS=1 (to Exit1)."""
        body = nbits - 1 if exit_shift else nbits
        cmd = bytearray()
        nbytes, rest = divmod(body, 8)
        data = tdi.to_bytes((nbits + 7) // 8 + 1, "little")
        if nbytes:
            cmd += bytes([0x39, (nbytes - 1) & 0xFF, (nbytes - 1) >> 8]) + data[:nbytes]
        if rest:
            cmd += bytes([0x3B, rest - 1, data[nbytes]])
        if exit_shift:
            last = (tdi >> (nbits - 1)) & 1
            cmd += bytes([0x6B, 0, 0x01 | (last << 7)])  # TMS=1 with TDI=last, read TDO
        cmd += b"\x87"
        self.write(bytes(cmd))
        raw = self.read(nbytes + (1 if rest else 0) + (1 if exit_shift else 0))
        val = int.from_bytes(raw[:nbytes], "little")
        pos = nbytes * 8
        if rest:
            val |= (raw[nbytes] >> (8 - rest)) << pos  # bit-mode reads shift in from the top
            pos += rest
        if exit_shift:
            val |= ((raw[-1] >> 7) & 1) << pos
        return val

    def reset(self) -> None:
        self.tms("111110")  # Test-Logic-Reset, then Run-Test/Idle

    def close(self) -> None:
        self.write(bytes([0x80, GPIO_IDLE, 0x00]))  # release outputs
        self._ctrl(SIO_SET_BITMODE, BITMODE_RESET << 8)
        usb.util.dispose_resources(self.dev)


def idcode_scan(ft: FT232H) -> list:
    """After reset every device holds IDCODE (or BYPASS) in DR; shift ones in and split."""
    ft.reset()
    ft.tms("100")  # Run-Test/Idle -> Select-DR -> Capture-DR -> Shift-DR
    raw = ft.shift(32 * 8, tdi=(1 << (32 * 8)) - 1)
    ft.tms("10")   # Exit1 -> Update-DR -> Run-Test/Idle
    ids, pos = [], 0
    while pos < 32 * 8 - 32:
        if (raw >> pos) & 1:
            word = (raw >> pos) & 0xFFFFFFFF
            if word == 0xFFFFFFFF:
                break
            ids.append(word)
            pos += 32
        else:
            ids.append(None)  # BYPASS device
            pos += 1
    return ids, raw


def main() -> None:
    if len(sys.argv) < 2 or sys.argv[1] != "idcode":
        sys.exit(__doc__)
    ft = FT232H()
    try:
        ids, raw = idcode_scan(ft)
    finally:
        ft.close()
    print(f"[*] TDO raw (first 64 bits): {raw & ((1 << 64) - 1):016x}")
    if not ids:
        sys.exit("[-] No device: TDO stuck at 1 (check TDO, TRST high and Blackfin power)")
    if raw & ((1 << 64) - 1) == 0:
        sys.exit("[-] TDO stuck at 0 (check wiring and that TRST is high)")
    for i, w in enumerate(ids):
        if w is None:
            print(f"    device {i}: BYPASS")
            continue
        print(f"    device {i}: IDCODE {w:08x} version {w >> 28:x} part {(w >> 12) & 0xFFFF:04x} "
              f"manufacturer {(w >> 1) & 0x7FF:03x}")


if __name__ == "__main__":
    main()
