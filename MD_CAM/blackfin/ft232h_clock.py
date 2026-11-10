#!/usr/bin/env python3
"""
ft232h_clock.py - Use the FT232H as a square-wave clock source on AD0 (e.g. into the Blackfin CLKIN).

The MPSSE clock runs from 60 MHz: f = 60 MHz / ((1 + div) * 2), so 15 MHz, 10 MHz, 7.5 MHz, ...
Only AD0 is driven; every other ADBUS pin is an input (with the FT232H internal pull-ups).
The clock is a stream of "clock 65536 x 8 bits, no data" (0x8F) commands kept queued, so it
runs with only short gaps between commands, and stops when the script exits.

Usage:
    python ft232h_clock.py [HZ] [SECONDS]     default 15000000 Hz for 60 s

Never drive CLKIN while the target is unpowered (input voltage must stay within VDDEXT).
"""
import sys
import time

from ft232h_jtag import FT232H, SIO_SET_BITMODE, BITMODE_RESET


def main() -> None:
    hz = int(float(sys.argv[1])) if len(sys.argv) > 1 else 15_000_000
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
    div = max(0, round(60_000_000 / (2 * hz)) - 1)
    actual = 60_000_000 / ((1 + div) * 2)

    ft = FT232H()
    try:
        ft.write(bytes([
            0x80, 0x00, 0x01,          # AD0 output (low), AD1..AD7 inputs
            0x86, div & 0xFF, div >> 8,
        ]))
        print(f"[+] Clock on AD0: {actual / 1e6:.3f} MHz for {seconds:.0f} s (Ctrl+C stops)")
        burst = bytes([0x8F, 0xFF, 0xFF]) * 8  # 8 x 524288 clocks per USB write
        t0 = time.time()
        while time.time() - t0 < seconds:
            ft.dev.write(0x02, burst, 60_000)  # blocks while the MPSSE is busy
    except KeyboardInterrupt:
        pass
    finally:
        ft._ctrl(SIO_SET_BITMODE, BITMODE_RESET << 8)
        ft.close()
        print("[+] Clock stopped")


if __name__ == "__main__":
    main()
