#!/usr/bin/env python3
"""
build_and_flash.py - Unified Build, Flash & RF Verification Automation

This script automates the full continuous integration & deployment pipeline
for the Verisure Door/Window Sensor (CC1110F32) and Gateway (ESP32-S2):

Pipeline Stages:
1. Compilation & Assembly (SDCC):
   - Compiles `VerisureDoorSensor/main.c` to MCS-51 8051 relocatable object (.rel)
   - Links object into Intel HEX format (.ihx) with SDCC linker
   - Packages HEX with `packihx` into standard Intel HEX (.hex)
   - Generates flat binary image (.bin) using `makebin -p`
2. Flashing & Verification (ESP_CC_Flasher HTTP REST API):
   - Wakes & synchronizes CC1110 debug interface via `POST /cc_init`
   - Uploads binary payload to flasher filesystem via `POST /edit`
   - Initiates flash programming via `POST /flash_cc?file=main.bin`
   - Streams and parses flasher status log until "Verify successful" is confirmed
   - Exits debug mode and resets the chip into live execution via `POST /cc_exit_dbg`
3. Real-Time RF Telemetry Verification:
   - Selects gateway serial port (auto-detects with interactive menu if omitted)
   - Connects to Gateway receiver over serial (115200 baud)
   - Listens for immediate live RF packet transmissions (Reed, Shock, Tamper)

Usage Examples:
    # 1. Automatic build, flash, and interactive port selection:
    python VerisureDoorSensor/build_and_flash.py

    # 2. Specify receiver port directly:
    python VerisureDoorSensor/build_and_flash.py --port COM9

    # 3. Compile only without flashing:
    python VerisureDoorSensor/build_and_flash.py --compile-only

    # 4. Flash without opening receiver monitor:
    python VerisureDoorSensor/build_and_flash.py --skip-monitor
"""

import sys
import os
import time
import subprocess
import argparse
import re
from typing import Optional
import serial
import requests
from requests.auth import HTTPBasicAuth
VERISURE_DIR = os.path.dirname(os.path.abspath(__file__))
WORKSPACE = os.path.dirname(VERISURE_DIR)
if WORKSPACE not in sys.path:
    sys.path.insert(0, WORKSPACE)   # port_selector.py is shared and lives in the repo root

from port_selector import select_com_port

# Workspace paths
BUILD_DIR = os.path.join(VERISURE_DIR, "build")
MAIN_C = os.path.join(VERISURE_DIR, "main.c")
IHX_FILE = os.path.join(BUILD_DIR, "main.ihx")
HEX_FILE = os.path.join(BUILD_DIR, "main.hex")
BIN_FILE = os.path.join(BUILD_DIR, "main.bin")

# CC Flasher Network configuration (mDNS default: http://cc.local)
DEFAULT_CC_HOST = os.environ.get("CC_HOST", "http://cc.local")
CC_USER = "admin"
CC_PASS = "admin"
RECEIVER_BAUD = 115200
GATEWAY_HTTP = os.environ.get("GATEWAY_HTTP", "http://verisure-gateway.local")   # the gateway's mDNS name
CONFIG_H = os.path.join(VERISURE_DIR, "config.h")


def counter_seed(gateway: str, no_seed: bool = False) -> int:
    """First counter block for a freshly flashed sensor (main.c CTR_SEED).

    Flashing erases the sensor's counter log, which would restart its packet counter at block 1,
    below what the gateway last accepted, so the gateway would drop it until 'replay reset <ID>'.
    Instead the gateway's last accepted counter (GET /api/replay?id=...) is read and the sensor
    starts two blocks above it (the old firmware may enter the next block before it is flashed).
    Returns 1 if the gateway does not know the sensor. Without the gateway the script stops (a
    counter that restarts reuses values); --no-seed flashes anyway.
    """
    if no_seed:
        print("[!] --no-seed: counter starts at block 1; run 'replay reset <ID>' on the gateway")
        return 1
    m = None
    try:
        with open(CONFIG_H, encoding="utf-8") as f:
            m = re.search(r'#define\s+DEVICE_ID\s+"([A-Za-z0-9]{8})"', f.read())
    except OSError:
        pass
    if not m:
        # the firmware would then build with config.h.example's ID and restart its counter
        print(f"[-] No DEVICE_ID in {CONFIG_H}: no counter seed. Generate config.h first, or use --no-seed.")
        sys.exit(1)
    dev = m.group(1)
    try:
        r = requests.get(f"{gateway}/api/replay", params={"id": dev}, timeout=5)
        data = r.json()
    except Exception as e:
        print(f"[-] Gateway {gateway} not reachable ({e}): no counter seed.")
        print("    Use --gateway / GATEWAY_HTTP, or --no-seed (counter restarts at block 1, needs 'replay reset').")
        sys.exit(1)
    if not data.get("known"):
        print(f"[+] Gateway has no counter for {dev} yet: counter starts at block 1")
        return 1
    seed = (int(data["counter"]) >> 8) + 2
    print(f"[+] Gateway's last counter for {dev}: {data['counter']}; sensor starts at block {seed}")
    return seed


def compile_firmware(ctr_seed: int = 1) -> bool:
    """
    Compile Verisure Door Sensor C code to binary using Small Device C Compiler (SDCC).

    Returns:
        bool: True on successful compilation, packaging, and binary generation.
    """
    print("=" * 60)
    print("1. COMPILING CC1110 FIRMWARE WITH SDCC")
    print("=" * 60)

    os.makedirs(BUILD_DIR, exist_ok=True)

    # 1. Compile C -> REL (Small memory model, 32KB flash limit, XRAM at 0xF000)
    cmd_compile = [
        "sdcc", "-mmcs51", "--model-small",
        "--code-size", "0x7800",   # 0x7800-0x7FFF: packet counter log (main.c)
        "--xram-loc", "0xF000", "--xram-size", "0x0E00",
        f"-DCTR_SEED={ctr_seed}UL",
        "-c", "main.c", "-o", "build/"
    ]
    print(f"[*] Running: {' '.join(cmd_compile)}")
    res = subprocess.run(cmd_compile, cwd=VERISURE_DIR, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[-] Compilation error:\n{res.stderr}\n{res.stdout}")
        return False

    # 2. Link REL -> IHX
    cmd_link = [
        "sdcc", "-mmcs51", "--model-small",
        "--code-size", "0x7800",   # 0x7800-0x7FFF: packet counter log (main.c)
        "--xram-loc", "0xF000", "--xram-size", "0x0E00",
        "build/main.rel", "-o", "build/main.ihx"
    ]
    print(f"[*] Running: {' '.join(cmd_link)}")
    res = subprocess.run(cmd_link, cwd=VERISURE_DIR, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[-] Linking error:\n{res.stderr}\n{res.stdout}")
        return False

    # 3. Pack IHX -> Intel HEX
    print("[*] Generating Intel HEX (packihx)...")
    with open(HEX_FILE, "w", encoding="utf-8") as f_hex:
        with open(IHX_FILE, "r", encoding="utf-8") as f_ihx:
            res = subprocess.run(["packihx"], stdin=f_ihx, stdout=f_hex, stderr=subprocess.PIPE, text=True)
            if res.returncode != 0:
                print(f"[-] packihx error: {res.stderr}")
                return False

    # 4. Pack IHX -> Flat Binary
    print("[*] Generating binary (makebin)...")
    res = subprocess.run(
        ["makebin", "-p", "build/main.ihx", "build/main.bin"],
        cwd=VERISURE_DIR,
        capture_output=True,
        text=True
    )
    if res.returncode != 0:
        print(f"[-] makebin error: {res.stderr}")
        return False

    size = os.path.getsize(BIN_FILE)
    print(f"[+] Build succeeded! Binary size: {size} bytes ({BIN_FILE})\n")
    return True


def get_latest_log_id(session: requests.Session, host: str) -> int:
    """Query ESP_CC_Flasher for the most recent log sequence number."""
    try:
        r = session.get(f"{host}/get_status", timeout=5)
        ids = [int(x) for x in re.findall(r'\{"(\d+)":', r.text)]
        return max(ids) if ids else 0
    except Exception:
        return 0


def flash_firmware(receiver_port: Optional[str] = None, cc_host: str = DEFAULT_CC_HOST, force_erase: bool = False, wait_contact: bool = False) -> Optional[serial.Serial]:
    """
    Upload and flash binary image to CC1110 via ESP_CC_Flasher HTTP API.

    Args:
        receiver_port (str, optional): Port to pre-open for live monitoring.
        cc_host (str): HTTP endpoint of CC Flasher.
        force_erase (bool): Force chip erase to unlock protected CC1110.
        wait_contact (bool): Loop until solid electrical contact with CC1110 is detected.

    Returns:
        Optional[serial.Serial]: Pre-opened serial connection or None.
    """
    print("=" * 60)
    print(f"2. FLASHING CC1110 VIA ESP_CC_FLASHER ({cc_host})")
    print("=" * 60)

    session = requests.Session()
    session.auth = HTTPBasicAuth(CC_USER, CC_PASS)

    # 1. Initialize CC debug interface
    if wait_contact:
        print("[*] Waiting for physical contact with J2 header (press and hold pins against pads)...")
    else:
        print(f"[*] Connecting to {cc_host}/cc_init...")

    init_text = ""
    status_val = 0
    t_probe_start = time.time()
    max_probe = 60 if wait_contact else 3

    while time.time() - t_probe_start < max_probe:
        try:
            resp = session.post(f"{cc_host}/cc_init", timeout=2)
            resp.raise_for_status()
            init_text = resp.text.strip()
            m = re.search(r'Status:\s*([0-9a-fA-F]+)', init_text)
            status_val = int(m.group(1), 16) if m else 0

            # Valid response: either chip ID 0104 or active debug status
            if "0104" in init_text or status_val != 0 or ("0000" not in init_text and "Status: 00" not in init_text):
                if wait_contact:
                    print(f"\n[+] Contact detected! {init_text}")
                else:
                    print(f"[+] {init_text}")
                break
            elif wait_contact:
                print(".", end="", flush=True)
        except Exception as e:
            if not wait_contact:
                print(f"[-] Failed to reach CC Flasher at {cc_host}: {e}")
                return None
        time.sleep(0.25)

    if wait_contact and "0104" not in init_text and status_val == 0:
        print("\n[-] Timed out waiting for electrical contact.")
        return None

    try:
        # Check if chip is locked (factory protected) or erase was requested
        status_val = 0
        m = re.search(r'Status:\s*([0-9a-fA-F]+)', init_text)
        if m:
            status_val = int(m.group(1), 16)

        is_locked = (status_val != 0xFF and status_val != 0 and (status_val & 0x04) != 0) or (status_val != 0 and status_val != 0xFF and "Device id: 0000" in init_text)
        if is_locked or force_erase:
            print("[*] Chip is locked (factory security) or erase requested. Unlocking via chip erase...")
            try:
                r_erase = session.post(f"{cc_host}/erase_cc", timeout=10)
                print(f"[+] {r_erase.text.strip()}")
                time.sleep(0.5)
                # Re-initialize to verify unlocked state
                resp = session.post(f"{cc_host}/cc_init", timeout=10)
                resp.raise_for_status()
                init_text = resp.text.strip()
                print(f"[+] Post-unlock init: {init_text}")
                m2 = re.search(r'Status:\s*([0-9a-fA-F]+)', init_text)
                if m2:
                    status_val = int(m2.group(1), 16)
            except Exception as e:
                print(f"[-] Failed during chip unlock: {e}")

        # Verify that we have genuine communication with a CC1110 (Chip ID 0104)
        if "0104" not in init_text and "01" not in init_text:
            print(f"\n[-] ERROR: No valid CC1110 chip communication ({init_text})!")
            print("    All bits returned 1s (FFFF) or 0s (0000). This means:")
            print("    - Debug Data (DD / Pad 4 -> GPIO 23) is disconnected/floating, OR")
            print("    - Debug Clock (DC / Pad 5 -> GPIO 19) is not pulsing the chip, OR")
            print("    - Ground (GND / Pad 7 -> GND) reference is missing.")
            print("    Please check the physical wire contact on Pad 4 and Pad 5.\n")
            return None
    except Exception as e:
        print(f"[-] Failed to reach CC Flasher at {cc_host}: {e}")
        return None

    # 2. Upload binary payload
    with open(BIN_FILE, "rb") as f:
        data = f.read()
    print(f"[*] Uploading /main.bin ({len(data)} bytes)...")
    files = {'data': ('/main.bin', data, 'application/octet-stream')}
    resp = session.post(f"{cc_host}/edit", files=files, timeout=30)
    resp.raise_for_status()
    print("[+] File uploaded successfully!")

    # 3. Retrieve baseline log ID
    last_log_id = get_latest_log_id(session, cc_host)
    print(f"[*] Latest log ID before flash start: {last_log_id}")

    # 4. Trigger flash command
    print("[*] Initiating flashing of /main.bin...")
    resp = session.post(f"{cc_host}/flash_cc", params={'file': 'main.bin'}, timeout=10)
    resp.raise_for_status()
    print(f"[+] Flash start response: {resp.text.strip()}")

    # 5. Monitor flashing progress and verification
    print("[*] Waiting for erase, programming, and verification to complete...")
    t0 = time.time()
    flash_complete = False
    flash_failed = False

    while time.time() - t0 < 45:
        try:
            r = session.get(f"{cc_host}/get_status", params={'received_log': last_log_id}, timeout=5)
            if r.status_code == 200 and r.text.strip():
                for m in re.finditer(r'\{"(\d+)":\s*"[^"]*":\s*"([^"]*)"\}', r.text):
                    lid = int(m.group(1))
                    msg = m.group(2)
                    if lid > last_log_id:
                        last_log_id = lid
                        print(f"    [{lid}] {msg}")
                        if "Verify successful" in msg:
                            flash_complete = True
                        elif "Verify failed" in msg:
                            flash_failed = True
                if flash_complete:
                    print("    -> [OK] Verify successful! Flashing completed and verified.")
                    break
                if flash_failed and "CC reset" in r.text:
                    print("    -> [-] Verify failed after attempt!")
                    break
        except Exception:
            pass
        time.sleep(0.5)

    if not flash_complete:
        print("[-] Warning: Flashing failed or verification timed out!")
        return None

    # Pre-open gateway serial connection before releasing chip reset
    s = None
    if receiver_port:
        print(f"[*] Opening receiver serial port {receiver_port} before reset...")
        try:
            s = serial.Serial(receiver_port, RECEIVER_BAUD, timeout=0.1)
        except Exception as e:
            print(f"[-] Failed to open {receiver_port}: {e}")
            s = None

    # 6. Exit debug and release reset line for live execution
    print("[*] Resetting CC1110 and resuming execution (cc_exit_dbg)...")
    try:
        resp = session.post(f"{cc_host}/cc_exit_dbg", timeout=10)
        print(f"[+] Response: {resp.text.strip()}\n")
    except Exception as e:
        print(f"[-] cc_exit_dbg error: {e}")

    return s


def monitor_receiver(s: Optional[serial.Serial], port: Optional[str] = None, duration: int = 30) -> bool:
    """
    Monitor serial log from gateway receiver for incoming sensor packets.

    Args:
        s (serial.Serial, optional): Pre-opened serial connection.
        port (str, optional): Port to open if s is None.
        duration (int): Monitoring duration in seconds.

    Returns:
        bool: True if an authenticated RF packet with valid CRC was captured.
    """
    if not port and s:
        port = s.port
    port_label = port or "Unknown port"

    print("=" * 60)
    print(f"3. MONITORING ESP32 RECEIVER ON {port_label} ({duration} s)")
    print("=" * 60)

    if s is None:
        if not port:
            print("[-] No serial port specified for monitoring.")
            return False
        try:
            s = serial.Serial(port, RECEIVER_BAUD, timeout=0.1)
        except Exception as e:
            print(f"[-] Failed to open {port}: {e}")
            return False

    t0 = time.time()
    packet_received = False

    try:
        while time.time() - t0 < duration:
            line = s.readline().decode('utf-8', errors='replace').strip()
            if line:
                print(f"  {line}")
                if "AUTHENTICATED" in line or ("RECEIVED" in line and "CRC: OK" in line):
                    packet_received = True
    finally:
        s.close()

    return packet_received


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Compile Verisure CC1110 firmware, flash via CC Flasher, and monitor RF link.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    parser.add_argument("--port", "-p", default=None,
                        help="Gateway receiver serial port (e.g. COM9, /dev/ttyACM0 on Linux). If omitted, interactive selector is shown.")
    parser.add_argument("--host", default=DEFAULT_CC_HOST,
                        help=f"ESP_CC_Flasher HTTP host (default: {DEFAULT_CC_HOST}).")
    parser.add_argument("--compile-only", "-c", action="store_true",
                        help="Only compile CC1110 firmware with SDCC without flashing.")
    parser.add_argument("--skip-monitor", "-s", action="store_true",
                        help="Flash CC1110 without opening receiver serial monitor.")
    parser.add_argument("--erase", "-e", action="store_true",
                        help="Perform full chip erase to unlock protected/factory CC1110 before flashing.")
    parser.add_argument("--wait-contact", "-w", action="store_true",
                        help="Wait in real-time until physical electrical contact with CC1110 is detected, then flash instantly.")
    parser.add_argument("--serial", default=None,
                        help="Flash over the flasher's USB serial port instead of Wi-Fi (e.g. COM12, /dev/ttyUSB0 on Linux); no monitor.")
    parser.add_argument("--gateway", default=GATEWAY_HTTP,
                        help=f"Gateway HTTP address for the counter seed (default: {GATEWAY_HTTP}, env GATEWAY_HTTP).")
    parser.add_argument("--no-seed", action="store_true",
                        help="Flash without the gateway's counter seed (counter restarts; needs 'replay reset <ID>').")
    parser.add_argument("--duration", "-d", type=int, default=30,
                        help="Monitoring verification duration in seconds (default: 30).")

    return parser.parse_args()


def main():
    args = parse_arguments()

    # 1. Compile firmware (a flashed build starts its packet counter above the gateway's)
    seed = 1 if args.compile_only else counter_seed(args.gateway, args.no_seed)
    if not compile_firmware(seed):
        sys.exit(1)

    if args.compile_only:
        print("[+] Compilation complete (--compile-only specified). Exiting.")
        print("[!] This build is NOT seeded (counter block 1): flash it with this script, not by hand,")
        print("    or the gateway drops the sensor until 'replay reset <ID>'.")
        return

    if args.serial:
        # same serial protocol as the camera (ESP_CC_Flasher src/serial_cmd.cpp)
        sys.path.insert(0, os.path.join(WORKSPACE, "MD_CAM"))
        import build_and_flash_md_cam as cam
        cam.BIN_PATH = BIN_FILE
        cam.flash_serial(args.serial)
        return

    # 2. Select receiver serial port if monitoring is enabled
    receiver_port = None
    if not args.skip_monitor:
        print("\n" + "-" * 65)
        print("[i] Flashing is done over Wi-Fi (http://cc.local via ESP_CC_Flasher).")
        print("    The COM port below is ONLY used to verify RF packets on your Gateway.")
        print("-" * 65)
        receiver_port = select_com_port(
            preferred_port=args.port or os.environ.get("RECEIVER_PORT"),
            hint_keyword="Gateway",
            prompt_title="Select Gateway Receiver Port (for Live RF Verification)"
        )

    # 3. Flash CC1110
    ser = flash_firmware(receiver_port=receiver_port, cc_host=args.host, force_erase=args.erase, wait_contact=args.wait_contact)
    if not ser:
        print("\n[-] Flashing failed. Monitoring aborted.")
        sys.exit(1)

    # 4. Monitor receiver
    if not args.skip_monitor:
        ok = monitor_receiver(ser, port=receiver_port, duration=args.duration)
        if ok:
            print("\n" + "=" * 60)
            print("[SUCCESS] PACKET RECEIVED WITH CRC: OK! RF LINK IS VERIFIED!")
            print("=" * 60)
        else:
            print("\n" + "=" * 60)
            print("[INFO] Monitoring completed.")
            print("=" * 60)
    else:
        print("[+] Flashing completed (--skip-monitor specified).")


if __name__ == "__main__":
    main()
