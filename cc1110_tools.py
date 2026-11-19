#!/usr/bin/env python3
"""
cc1110_tools.py - Shared helpers of the CC1110 build-and-flash scripts
(VerisureDoorSensor/build_and_flash.py and MD_CAM/build_and_flash_md_cam.py).

* config_device_id()  the DEVICE_ID of a generated config.h
* write_config()      config.h for a device ID from the master key in 868Receiver/src/secrets.h
* counter_seed()      first packet counter block of a freshly flashed device (asks the gateway)
* flash_serial()      flash a binary over the ESP_CC_Flasher's USB serial port
* replay_counter(), wait_for_device()  check that the gateway accepts the flashed device
"""
import os
import re
import sys
import time
from typing import Optional

import requests

import derive_key

key_check_value = derive_key.key_check_value


def config_device_id(config_h: str) -> Optional[str]:
    """DEVICE_ID of a generated config.h, or None if there is none (or only the example)."""
    try:
        with open(config_h, encoding="utf-8") as f:
            m = re.search(r'#define\s+DEVICE_ID\s+"([A-Za-z0-9]{8})"', f.read())
    except OSError:
        return None
    return m.group(1) if m else None


def write_config(device_id: str, config_h: str, master_hex: Optional[str] = None) -> str:
    """Write config.h for device_id (label form like "2JHL 2MVN" is fine); returns the clean ID.
    The master key comes from secrets.h unless given; only its check value is printed."""
    try:
        dev = derive_key.clean_device_id(device_id)
    except ValueError as e:
        sys.exit(f"[-] {e}")
    mk = master_hex or derive_key.get_master_key_from_secrets()
    if not mk:
        sys.exit("[-] No master key: put MASTER_KEY_HEX in 868Receiver/src/secrets.h")
    key = bytes.fromhex(mk)
    print(f"[+] Master key check value {key_check_value(key)} "
          "(must match 'Master Key : set (check value ...)' in the gateway's status command)")
    if key == bytes.fromhex(derive_key.DEFAULT_MASTER_KEY_HEX):
        print("[!] WARNING: secrets.h has the PUBLIC example master key: anyone can make packets with it.")
        print("    Generate your own: python derive_key.py --generate-master")
    derive_key.write_config_header(dev, derive_key.derive_device_key(key, dev), config_h)
    return dev


def counter_seed(gateway: str, device_id: str, no_seed: bool = False) -> int:
    """First counter block for a freshly flashed device (main.c CTR_SEED).

    Flashing erases the device's counter log, which would restart its packet counter at block 1,
    below what the gateway last accepted, so the gateway would drop it until 'replay reset <ID>'.
    Instead the gateway's last accepted counter (GET /api/replay?id=...) is read and the device
    starts two blocks above it (the old firmware keeps sending until it is flashed and may enter
    the next block meanwhile). Returns 1 if the gateway does not know the device.

    Without the gateway the script stops: a counter that restarts at block 1 reuses values (the
    gateway drops them, and for the camera its recorded poll answers would fit again). --no-seed
    flashes anyway.
    """
    if no_seed:
        print(f"[!] --no-seed: counter starts at block 1; run 'replay reset {device_id}' on the gateway")
        return 1
    try:
        data = replay_query(gateway, device_id)
    except Exception as e:
        print(f"[-] No counter seed from the gateway {gateway}: {e}")
        print("    Use --gateway http://<gateway IP> or http://verisure-gateway.local (the gateway answers")
        print("    only to its own names), or --no-seed (counter restarts at block 1, then run")
        print(f"    'replay reset {device_id}' on the gateway).")
        sys.exit(1)
    if not data["known"]:
        print(f"[+] Gateway has no counter for {device_id} yet: counter starts at block 1")
        return 1
    seed = (int(data["counter"]) >> 8) + 2
    print(f"[+] Gateway's last counter for {device_id}: {data['counter']}; device starts at block {seed}")
    return seed


def replay_query(gateway: str, device_id: str) -> dict:
    """GET /api/replay?id=... (public). Raises on any error, also on a refusal such as 403
    bad_host (the gateway answers only to its IP and verisure-gateway[.local]), so a refused
    request is never mistaken for "no counter yet"."""
    r = requests.get(f"{gateway}/api/replay", params={"id": device_id}, timeout=5)
    if r.status_code != 200:
        raise RuntimeError(f"HTTP {r.status_code} {r.text[:80]}")
    data = r.json()
    if "known" not in data or "counter" not in data:
        raise RuntimeError(f"unexpected answer {r.text[:80]}")
    return data


def replay_counter(gateway: str, device_id: str) -> Optional[int]:
    """The gateway's last accepted counter of a device, 0 if it has none yet, None on any error."""
    try:
        data = replay_query(gateway, device_id)
    except Exception:
        return None
    return int(data["counter"]) if data["known"] else 0


def wait_for_device(gateway: str, device_id: str, seed: int, timeout: float = 90) -> bool:
    """After flashing a build with counter seed 'seed' (block): wait until the gateway has accepted
    a packet of device_id from that block or later. The old firmware's counters are below the seed
    block and only an authenticated packet moves the gateway's counter, so this proves the new
    firmware's key and seed (the device merely being listed would not)."""
    need = seed << 8
    print(f"[*] Waiting up to {timeout:.0f} s for an authenticated packet from {device_id} "
          f"(counter >= {need}) at {gateway} ...")
    t0 = time.time()
    now = None
    while time.time() - t0 < timeout:
        now = replay_counter(gateway, device_id)
        if now is not None and now >= need:
            print(f"[+] {device_id} authenticated on the gateway (counter {now})")
            return True
        time.sleep(2)
    if now is None:
        print(f"[-] Gateway {gateway} did not answer: check the gateway log for {device_id} yourself")
    else:
        print(f"[-] No authenticated packet from {device_id} within {timeout:.0f} s (gateway counter {now}, "
              f"need >= {need}): wrong key, out of range, or not powered; check the gateway log")
    return False


def flash_serial(port: str, bin_path: str, allow_locked: bool = False) -> None:
    """Flash over the flasher's USB serial port (ESP_CC_Flasher src/serial_cmd.cpp), no Wi-Fi.
    DTR/RTS stay low so opening the port does not reset the ESP32. Exits on any failure."""
    import zlib
    import serial

    with open(bin_path, "rb") as f:
        data = f.read()
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.5
    s.dtr = False
    s.rts = False
    s.open()

    def answer(timeout: float) -> str:
        t0 = time.time()
        while time.time() - t0 < timeout:
            line = s.readline().decode("utf-8", "replace").strip()
            if line.startswith("#"):
                return line
            if line:
                print(f"    {line}")
        return "#TIMEOUT"

    try:
        s.reset_input_buffer()
        s.write(b"PING\n")
        if answer(3) != "#PONG":
            print(f"[-] Flasher on {port} does not answer PING (needs the serial command firmware).")
            sys.exit(1)
        s.write(b"INIT\n")
        init = answer(10)
        print(f"[+] {init}")
        parts = init.split()
        if len(parts) != 3 or parts[0] != "#INIT" or parts[1] != "0104":
            print("[-] Target is not responding with CC1110F32 (0104). Check wires/battery.")
            sys.exit(1)
        if int(parts[2], 16) & 0x04:
            if not allow_locked:
                print("[-] The CC1110 is debug-locked (a factory device). Flashing erases the whole chip and")
                print("    with it the factory firmware, for good (it cannot be read back): rerun with --erase.")
                sys.exit(1)
            print("[!] Debug-locked CC1110 (--erase): the chip erase before flashing wipes the factory firmware.")
        print(f"[*] Flashing {len(data)} bytes over {port}...")
        s.write(f"FLASH {len(data)} {zlib.crc32(data) & 0xFFFFFFFF:08x}\n".encode())
        if answer(5) != "#READY":
            print("[-] Flasher did not get ready.")
            sys.exit(1)
        s.write(data)
        got = answer(15)
        if got != "#GOT":
            print(f"[-] Transfer failed: {got}")
            sys.exit(1)
        done = answer(90)
        if done != "#DONE OK":
            print(f"[-] Flashing failed: {done}")
            sys.exit(1)
        print("[+] FLASHING & VERIFICATION SUCCEEDED! (CC1110 reset and running)")
    finally:
        s.close()
