#!/usr/bin/env python3
"""
read_u3.py - Läs U3 (EN25F40) via ESP32-läsaren två gånger, verifiera CRC och jämför.

Användning:
    python read_u3.py --out ../../../scratch/u3_board2.bin [--port COM7]
"""
import argparse
import os
import sys
import time
import zlib

import serial

REPO_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
sys.path.insert(0, REPO_DIR)
import port_selector  # noqa: E402

EXPECTED_ID = "1c 31 13"


def read_line(ser: serial.Serial, timeout: float = 5.0) -> str:
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        b = ser.read(1)
        if not b:
            continue
        if b == b"\n":
            return buf.decode(errors="replace").strip()
        buf += b
    raise TimeoutError(f"Inget svar från läsaren (fick {buf!r})")


def read_id(ser: serial.Serial) -> str:
    ser.reset_input_buffer()
    ser.write(b"I")
    line = read_line(ser)
    if not line.startswith("ID "):
        raise RuntimeError(f"Oväntat svar: {line!r}")
    return line[3:]


def read_dump(ser: serial.Serial) -> bytes:
    ser.reset_input_buffer()
    ser.write(b"R")
    head = read_line(ser)
    if not head.startswith("DUMP "):
        raise RuntimeError(f"Oväntat svar: {head!r}")
    length = int(head.split()[1])
    data = bytearray()
    t0 = time.time()
    while len(data) < length:
        chunk = ser.read(length - len(data))
        if not chunk and time.time() - t0 > 120:
            raise TimeoutError(f"Dumpen avbröts efter {len(data)} bytes")
        data += chunk
        print(f"\r  {len(data) * 100 // length:3d} %", end="", flush=True)
    print()
    read_line(ser)  # tom rad före CRC
    crc_line = read_line(ser)
    crc = int(crc_line.split()[1], 16)
    if zlib.crc32(data) & 0xFFFFFFFF != crc:
        raise RuntimeError("CRC stämmer inte - överföringen är trasig")
    return bytes(data)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--port")
    args = ap.parse_args()

    port = port_selector.select_com_port(args.port, "ESP32", "Välj ESP32 U3-läsare")
    if not port:
        sys.exit(1)
    with serial.Serial(port, 921600, timeout=0.5) as ser:
        time.sleep(1.0)
        jedec = read_id(ser)
        print(f"[*] JEDEC ID: {jedec}")
        if jedec != EXPECTED_ID:
            print(f"[-] Förväntade {EXPECTED_ID} (EN25F40). Kontrollera klämma, pin 1 och att kortet är strömlöst.")
            sys.exit(1)

        print("[*] Läsning 1 ...")
        d1 = read_dump(ser)
        print("[*] Läsning 2 ...")
        d2 = read_dump(ser)

    if d1 != d2:
        diff = sum(1 for a, b in zip(d1, d2) if a != b)
        print(f"[-] Läsningarna skiljer sig åt i {diff} bytes - kontakten är instabil, läs om.")
        sys.exit(1)

    with open(args.out, "wb") as f:
        f.write(d1)
    used = sum(1 for i in range(0, len(d1), 256) if d1[i:i + 256] != b"\xff" * 256)
    print(f"[+] Två identiska läsningar sparade till {args.out}")
    print(f"[+] Sidor som inte är tomma (0xFF): {used} av {len(d1) // 256}")


if __name__ == "__main__":
    main()
