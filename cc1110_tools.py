#!/usr/bin/env python3
"""
cc1110_tools.py - Shared helpers of the CC1110 build-and-flash scripts
(VerisureDoorSensor/build_and_flash.py and MD_CAM/build_and_flash_md_cam.py).

* config_device_id()  the DEVICE_ID of a generated config.h
* write_config()      config.h for a device ID from the master key in 868Receiver/src/secrets.h
* counter_seed()      first packet counter block of a freshly flashed device (asks the gateway)
* flash_serial()      flash a binary over the ESP_CC_Flasher's USB serial port
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
        r = requests.get(f"{gateway}/api/replay", params={"id": device_id}, timeout=5)
        data = r.json()
    except Exception as e:
        print(f"[-] Gateway {gateway} not reachable ({e}): no counter seed.")
        print("    Use --gateway http://<ip> (or GATEWAY_HTTP), or --no-seed (counter restarts at block 1,")
        print(f"    then run 'replay reset {device_id}' on the gateway).")
        sys.exit(1)
    if not data.get("known"):
        print(f"[+] Gateway has no counter for {device_id} yet: counter starts at block 1")
        return 1
    seed = (int(data["counter"]) >> 8) + 2
    print(f"[+] Gateway's last counter for {device_id}: {data['counter']}; device starts at block {seed}")
    return seed


def flash_serial(port: str, bin_path: str) -> None:
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
            print("[-] Target has DEBUG_LOCKED=1 (locked reference board). Aborting.")
            sys.exit(1)
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
