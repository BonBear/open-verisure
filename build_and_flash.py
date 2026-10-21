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
    python build_and_flash.py

    # 2. Specify receiver port directly:
    python build_and_flash.py --port COM9

    # 3. Compile only without flashing:
    python build_and_flash.py --compile-only

    # 4. Flash without opening receiver monitor:
    python build_and_flash.py --skip-monitor
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
from port_selector import select_com_port

# Workspace paths
WORKSPACE = os.path.dirname(os.path.abspath(__file__))
VERISURE_DIR = os.path.join(WORKSPACE, "VerisureDoorSensor")
BUILD_DIR = os.path.join(VERISURE_DIR, "build")
MAIN_C = os.path.join(VERISURE_DIR, "main.c")
IHX_FILE = os.path.join(BUILD_DIR, "main.ihx")
HEX_FILE = os.path.join(BUILD_DIR, "main.hex")
BIN_FILE = os.path.join(BUILD_DIR, "main.bin")

# CC Flasher Network configuration
DEFAULT_CC_HOST = os.environ.get("CC_HOST", "http://192.168.0.80")
CC_USER = "admin"
CC_PASS = "admin"
RECEIVER_BAUD = 115200


def compile_firmware() -> bool:
    """
    Compile Verisure Door Sensor C code to binary using Small Device C Compiler (SDCC).

    Returns:
        bool: True on successful compilation, packaging, and binary generation.
    """
    print("=" * 60)
    print("1. KOMPILERAR CC1110 FIRMWARE MED SDCC")
    print("=" * 60)

    os.makedirs(BUILD_DIR, exist_ok=True)

    # 1. Compile C -> REL (Small memory model, 32KB flash limit, XRAM at 0xF000)
    cmd_compile = [
        "sdcc", "-mmcs51", "--model-small",
        "--code-size", "0x8000",
        "--xram-loc", "0xF000", "--xram-size", "0x0E00",
        "-c", "main.c", "-o", "build/"
    ]
    print(f"[*] Kör: {' '.join(cmd_compile)}")
    res = subprocess.run(cmd_compile, cwd=VERISURE_DIR, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[-] Kompileringsfel:\n{res.stderr}\n{res.stdout}")
        return False

    # 2. Link REL -> IHX
    cmd_link = [
        "sdcc", "-mmcs51", "--model-small",
        "--code-size", "0x8000",
        "--xram-loc", "0xF000", "--xram-size", "0x0E00",
        "build/main.rel", "-o", "build/main.ihx"
    ]
    print(f"[*] Kör: {' '.join(cmd_link)}")
    res = subprocess.run(cmd_link, cwd=VERISURE_DIR, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[-] Länkningsfel:\n{res.stderr}\n{res.stdout}")
        return False

    # 3. Pack IHX -> Intel HEX
    print("[*] Genererar Intel HEX (packihx)...")
    with open(HEX_FILE, "w", encoding="utf-8") as f_hex:
        with open(IHX_FILE, "r", encoding="utf-8") as f_ihx:
            res = subprocess.run(["packihx"], stdin=f_ihx, stdout=f_hex, stderr=subprocess.PIPE, text=True)
            if res.returncode != 0:
                print(f"[-] packihx fel: {res.stderr}")
                return False

    # 4. Pack IHX -> Flat Binary
    print("[*] Genererar binär (makebin)...")
    res = subprocess.run(
        ["makebin", "-p", "build/main.ihx", "build/main.bin"],
        cwd=VERISURE_DIR,
        capture_output=True,
        text=True
    )
    if res.returncode != 0:
        print(f"[-] makebin fel: {res.stderr}")
        return False

    size = os.path.getsize(BIN_FILE)
    print(f"[+] Bygge lyckades! Binärstorlek: {size} bytes ({BIN_FILE})\n")
    return True


def get_latest_log_id(session: requests.Session, host: str) -> int:
    """Query ESP_CC_Flasher for the most recent log sequence number."""
    try:
        r = session.get(f"{host}/get_status", timeout=5)
        ids = [int(x) for x in re.findall(r'\{"(\d+)":', r.text)]
        return max(ids) if ids else 0
    except Exception:
        return 0


def flash_firmware(receiver_port: Optional[str] = None, cc_host: str = DEFAULT_CC_HOST) -> Optional[serial.Serial]:
    """
    Upload and flash binary image to CC1110 via ESP_CC_Flasher HTTP API.

    Args:
        receiver_port (str, optional): Port to pre-open for live monitoring.
        cc_host (str): HTTP endpoint of CC Flasher.

    Returns:
        Optional[serial.Serial]: Pre-opened serial connection or None.
    """
    print("=" * 60)
    print(f"2. FLASHAR CC1110 VIA ESP_CC_FLASHER ({cc_host})")
    print("=" * 60)

    session = requests.Session()
    session.auth = HTTPBasicAuth(CC_USER, CC_PASS)

    # 1. Initialize CC debug interface
    print(f"[*] Ansluter till {cc_host}/cc_init...")
    try:
        resp = session.post(f"{cc_host}/cc_init", timeout=10)
        resp.raise_for_status()
        print(f"[+] {resp.text.strip()}")
    except Exception as e:
        print(f"[-] Kunde inte nå CC Flasher på {cc_host}: {e}")
        return None

    # 2. Upload binary payload
    with open(BIN_FILE, "rb") as f:
        data = f.read()
    print(f"[*] Laddar upp /main.bin ({len(data)} bytes)...")
    files = {'data': ('/main.bin', data, 'application/octet-stream')}
    resp = session.post(f"{cc_host}/edit", files=files, timeout=30)
    resp.raise_for_status()
    print("[+] Fil uppladdad!")

    # 3. Retrieve baseline log ID
    last_log_id = get_latest_log_id(session, cc_host)
    print(f"[*] Senaste log-ID före flashstart: {last_log_id}")

    # 4. Trigger flash command
    print("[*] Startar flashning av /main.bin...")
    resp = session.post(f"{cc_host}/flash_cc", params={'file': 'main.bin'}, timeout=10)
    resp.raise_for_status()
    print(f"[+] Flash-start svar: {resp.text.strip()}")

    # 5. Monitor flashing progress and verification
    print("[*] Väntar på att radering, flashning och verifiering slutförs...")
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
                    print("    -> [OK] Verify successful! Flashning slutförd och verifierad.")
                    break
                if flash_failed and "CC reset" in r.text:
                    print("    -> [-] Verify failed efter försök!")
                    break
        except Exception:
            pass
        time.sleep(0.5)

    if not flash_complete:
        print("[-] Varning: Flashning lyckades inte eller verifiering uteblev!")
        return None

    # Pre-open gateway serial connection before releasing chip reset
    s = None
    if receiver_port:
        print(f"[*] Öppnar mottagarens serieport {receiver_port} före reset...")
        try:
            s = serial.Serial(receiver_port, RECEIVER_BAUD, timeout=0.1)
        except Exception as e:
            print(f"[-] Kunde inte öppna {receiver_port}: {e}")
            s = None

    # 6. Exit debug and release reset line for live execution
    print("[*] Återställer CC1110 och startar exekvering (cc_exit_dbg)...")
    try:
        resp = session.post(f"{cc_host}/cc_exit_dbg", timeout=10)
        print(f"[+] Svar: {resp.text.strip()}\n")
    except Exception as e:
        print(f"[-] cc_exit_dbg fel: {e}")

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
    port_label = port or "Okänd port"

    print("=" * 60)
    print(f"3. ÖVERVAKAR ESP32-MOTTAGAREN PÅ {port_label} ({duration} s)")
    print("=" * 60)

    if s is None:
        if not port:
            print("[-] Ingen serieport angiven för övervakning.")
            return False
        try:
            s = serial.Serial(port, RECEIVER_BAUD, timeout=0.1)
        except Exception as e:
            print(f"[-] Kunde inte öppna {port}: {e}")
            return False

    t0 = time.time()
    packet_received = False

    try:
        while time.time() - t0 < duration:
            line = s.readline().decode('utf-8', errors='replace').strip()
            if line:
                print(f"  {line}")
                if "AUTHENTICATED" in line or ("MOTTAGET" in line and "CRC: OK" in line):
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
                        help="Gateway receiver serial port (e.g. COM9). If omitted, interactive selector is shown.")
    parser.add_argument("--host", default=DEFAULT_CC_HOST,
                        help=f"ESP_CC_Flasher HTTP host (default: {DEFAULT_CC_HOST}).")
    parser.add_argument("--compile-only", "-c", action="store_true",
                        help="Only compile CC1110 firmware with SDCC without flashing.")
    parser.add_argument("--skip-monitor", "-s", action="store_true",
                        help="Flash CC1110 without opening receiver serial monitor.")
    parser.add_argument("--duration", "-d", type=int, default=30,
                        help="Monitoring verification duration in seconds (default: 30).")

    return parser.parse_args()


def main():
    args = parse_arguments()

    # 1. Compile firmware
    if not compile_firmware():
        sys.exit(1)

    if args.compile_only:
        print("[+] Kompilering slutförd (--compile-only angivet). Avslutar.")
        return

    # 2. Select receiver serial port if monitoring is enabled
    receiver_port = None
    if not args.skip_monitor:
        receiver_port = select_com_port(
            preferred_port=args.port or os.environ.get("RECEIVER_PORT"),
            hint_keyword="Gateway",
            prompt_title="Välj Gateway Mottagarport"
        )

    # 3. Flash CC1110
    ser = flash_firmware(receiver_port=receiver_port, cc_host=args.host)
    if not ser and not args.skip_monitor and not receiver_port:
        sys.exit(1)

    # 4. Monitor receiver
    if not args.skip_monitor:
        ok = monitor_receiver(ser, port=receiver_port, duration=args.duration)
        if ok:
            print("\n" + "=" * 60)
            print("[SUCCESS] PAKET MOTTAGET MED CRC: OK! RF-LÄNKEN ÄR VERIFIERAD!")
            print("=" * 60)
        else:
            print("\n" + "=" * 60)
            print("[INFO] Övervakning slutförd.")
            print("=" * 60)
    else:
        print("[+] Flashning slutförd (--skip-monitor angivet).")


if __name__ == "__main__":
    main()
