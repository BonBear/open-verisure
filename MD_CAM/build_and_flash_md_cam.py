#!/usr/bin/env python3
"""
build_and_flash_md_cam.py - Sync KDF with Gateway HTTP API, Compile, Flash MD_CAM & Verify Telemetry
"""
import os
import re
import sys
import time
import subprocess
import requests
from requests.auth import HTTPBasicAuth

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.dirname(BASE_DIR)
if REPO_DIR not in sys.path:
    sys.path.insert(0, REPO_DIR)

import derive_key

CC_HOST = os.environ.get("CC_HOST", "http://192.168.0.80")
CC_USER = "admin"
CC_PASS = "admin"
GATEWAY_HTTP = os.environ.get("GATEWAY_HTTP", "http://192.168.0.142")
DEVICE_ID = "2NCEKS3T"

FW_DIR = os.path.join(BASE_DIR, "firmware")
BUILD_DIR = os.path.join(FW_DIR, "build")
CONFIG_H = os.path.join(FW_DIR, "config.h")
BIN_PATH = os.path.join(BUILD_DIR, "main.bin")
REMOTE_NAME = "md_cam.bin"


def sync_key_from_gateway() -> bool:
    """Derive config.h from the master key in secrets.h. Older gateways also returned their
    active key in /api/status (in memory only, never printed); newer ones do not."""
    mk_hex = derive_key.get_master_key_from_secrets() or derive_key.DEFAULT_MASTER_KEY_HEX
    key_changed = False
    try:
        r = requests.get(f"{GATEWAY_HTTP}/api/status", timeout=5)
        if r.status_code == 200:
            data = r.json()
            gw_mk = (data.get("config", {}).get("master_key") or "").strip().upper()
            if len(gw_mk) == 32:
                matches_secrets = (gw_mk == mk_hex.upper())
                print(f"[+] Queried Gateway at {GATEWAY_HTTP}/api/status (NVS key matches secrets.h: {matches_secrets})")
                if not matches_secrets:
                    key_changed = True
                mk_hex = gw_mk
            else:
                # newer gateways never send the key back (only "master_key_set")
                print("[+] Gateway does not expose its master key; using the key from secrets.h")
            sensors = data.get("sensors", [])
            print(f"[+] Currently registered sensors on Gateway: {[s.get('id') for s in sensors]}")
    except Exception as e:
        print(f"[!] Could not query {GATEWAY_HTTP}/api/status ({e}), using secrets.h")

    print(f"[+] Master key check value {key_check_value(bytes.fromhex(mk_hex))} "
          "(must match 'Master Key : set (check value ...)' in the gateway's status command)")
    dk = derive_key.derive_device_key(bytes.fromhex(mk_hex), DEVICE_ID)
    derive_key.write_config_header(DEVICE_ID, dk, CONFIG_H)
    return key_changed


def key_check_value(key: bytes) -> str:
    """First 3 bytes of AES(key, 0^16), as the gateway prints it: compares keys without showing them."""
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    enc = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    return (enc.update(bytes(16)) + enc.finalize())[:3].hex().upper()


def compile_firmware() -> None:
    os.makedirs(BUILD_DIR, exist_ok=True)
    cmd_c = [
        "sdcc", "-mmcs51", "--model-large",
        "--code-size", "0x8000", "--xram-loc", "0xF000", "--xram-size", "0x0E00",
        "-c", "main.c", "-o", "build/"
    ]
    res = subprocess.run(cmd_c, cwd=FW_DIR, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[-] Compile error:\n{res.stderr}\n{res.stdout}")
        sys.exit(1)

    cmd_l = [
        "sdcc", "-mmcs51", "--model-large",
        "--code-size", "0x8000", "--xram-loc", "0xF000", "--xram-size", "0x0E00",
        "build/main.rel", "-o", "build/main.ihx"
    ]
    res = subprocess.run(cmd_l, cwd=FW_DIR, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[-] Link error:\n{res.stderr}\n{res.stdout}")
        sys.exit(1)

    res = subprocess.run(["makebin", "-p", "build/main.ihx", "build/main.bin"], cwd=FW_DIR, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[-] makebin error:\n{res.stderr}")
        sys.exit(1)
    print(f"[+] Compiled {BIN_PATH} ({os.path.getsize(BIN_PATH)} bytes)")


def get_latest_log_id(session: requests.Session, host: str) -> int:
    try:
        r = session.get(f"{host}/get_status", timeout=5)
        ids = [int(x) for x in re.findall(r'\{"(\d+)":', r.text)]
        return max(ids) if ids else 0
    except Exception:
        return 0


def flash_serial(port: str) -> None:
    """Flash over the flasher's USB serial port (ESP_CC_Flasher src/serial_cmd.cpp), no WiFi.
    DTR/RTS stay low so opening the port does not reset the ESP32."""
    import zlib
    import serial

    with open(BIN_PATH, "rb") as f:
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


def main():
    sync_key_from_gateway()
    compile_firmware()
    if "--serial" in sys.argv:
        flash_serial(sys.argv[sys.argv.index("--serial") + 1])
    else:
        flash_http()
    verify_on_gateway()


def flash_http():
    session = requests.Session()
    session.auth = HTTPBasicAuth(CC_USER, CC_PASS)

    # 1. Upload binary FIRST
    with open(BIN_PATH, "rb") as f:
        data = f.read()
    print(f"[*] Uploading /{REMOTE_NAME} ({len(data)} bytes)...")
    files = {"data": (f"/{REMOTE_NAME}", data, "application/octet-stream")}
    r_up = session.post(f"{CC_HOST}/edit", files=files, timeout=20)
    r_up.raise_for_status()
    print("[+] Upload complete!")

    last_log_id = get_latest_log_id(session, CC_HOST)

    # 2. Verify target is connected and UNLOCKED
    print(f"[*] Checking target chip via {CC_HOST}/cc_init ...")
    r_init = session.post(f"{CC_HOST}/cc_init", timeout=15)
    init_text = r_init.text.strip()
    print(f"[+] {init_text}")

    m = re.search(r"Status:\s*([0-9a-fA-F]+)", init_text)
    status_val = int(m.group(1), 16) if m else 0xFF
    if "0104" not in init_text:
        print("[-] Target is not responding with CC1110F32 (0104). Check wires/battery.")
        sys.exit(1)
    if (status_val & 0x04) != 0:
        print("[-] Target has DEBUG_LOCKED=1 (this is the locked reference board!). Aborting to protect reference board.")
        sys.exit(1)

    # 3. Flash and verify
    print(f"[*] Flashing /{REMOTE_NAME} to CC1110F32...")
    r_fl = session.post(f"{CC_HOST}/flash_cc", params={"file": REMOTE_NAME}, timeout=10)
    print(f"[+] {r_fl.text.strip()}")

    t0 = time.time()
    flash_ok = False
    while time.time() - t0 < 45:
        try:
            r = session.get(f"{CC_HOST}/get_status", params={"received_log": last_log_id}, timeout=5)
            if r.status_code == 200 and r.text.strip():
                for m in re.finditer(r'\{"(\d+)":\s*"(\d+)":\s*"([^"]*)"\}', r.text):
                    lid = int(m.group(1))
                    chan = int(m.group(2))
                    msg = m.group(3)
                    if lid > last_log_id:
                        last_log_id = lid
                        if chan == 0:
                            print(f"    [{lid}] {msg}")
                            if "Verify successful" in msg:
                                flash_ok = True
                            elif "Verify failed" in msg:
                                print("[-] Flash verification failed!")
                                sys.exit(1)
                if flash_ok:
                    break
        except Exception:
            pass
        time.sleep(0.4)

    if not flash_ok:
        print("[-] Timed out waiting for Verify successful.")
        sys.exit(1)

    print("[+] FLASHING & VERIFICATION SUCCEEDED!")
    try:
        session.post(f"{CC_HOST}/cc_exit_dbg", timeout=5)
    except Exception:
        pass


def verify_on_gateway():
    # 4. Poll Gateway HTTP /api/status to verify 2NCEKT3X registration!
    print(f"[*] Polling {GATEWAY_HTTP}/api/status for sensor {DEVICE_ID} telemetry...")
    for _ in range(10):
        time.sleep(1.5)
        try:
            r = requests.get(f"{GATEWAY_HTTP}/api/status", timeout=5)
            if r.status_code == 200:
                data = r.json()
                for s in data.get("sensors", []):
                    if s.get("id") == DEVICE_ID:
                        p0_pu = s.get("p0", 0)
                        p0_pd = s.get("p1", 0)
                        p1_pu = s.get("p2", 0)
                        p2_pk = s.get("rst", 0)
                        print("\n" + "=" * 65)
                        print(f"[SUCCESS] {DEVICE_ID} AUTHENTICATED ON GATEWAY & HOME ASSISTANT!")
                        print(f"  Motion (PIR) : {'MOTION DETECTED [ALARM]' if s.get('magnet') else 'Clear (Idle)'}")
                        print(f"  Tamper (SW3) : {'TAMPER!' if s.get('button') else 'Secured'}")
                        print(f"  Battery      : {s.get('battery_mv')} mV ({s.get('battery_pct')}%)")
                        print(f"  Signal       : RSSI {s.get('rssi')} dBm | LQI {s.get('lqi')} | Seq #{s.get('seq')}")
                        print(f"  GPIO Scan    : P0_PU=0x{p0_pu:02X} P0_PD=0x{p0_pd:02X} | P1_PU=0x{p1_pu:02X} | P2=0x{p2_pk:02X}")
                        print("=" * 65)
                        return
        except Exception:
            pass
    print("[!] Sensor not yet seen in /api/status; check Gateway logs.")


if __name__ == "__main__":
    main()
