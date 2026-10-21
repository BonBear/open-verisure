#!/usr/bin/env python3
"""
flash_cc1110.py - Standalone CLI Flasher for CC1110 via ESP_CC_Flasher Web API

Communicates with the ESP32 CC-Flasher board over HTTP to flash, verify,
and run firmware on Texas Instruments CC1110 / CC2510 SoC chips.

ESP_CC_Flasher HTTP API Endpoints:
- `POST /cc_init`    : Powers up and synchronizes the 2-wire debug interface (DC/DD)
- `POST /edit`       : Uploads firmware file (.bin / .hex) to SPIFFS filesystem
- `POST /flash_cc`   : Flashes specified file from SPIFFS to CC1110 flash memory
- `GET  /get_status` : Queries real-time flash programming and verification logs
- `POST /cc_exit_dbg`: Exits debug mode and releases RESET_N to start firmware execution

Usage:
    python flash_cc1110.py VerisureDoorSensor/build/main.bin
    python flash_cc1110.py VerisureDoorSensor/build/main.bin --host http://192.168.0.80
"""

import sys
import os
import time
import argparse
import requests
from requests.auth import HTTPBasicAuth

DEFAULT_HOST = os.environ.get("CC_HOST", "http://192.168.0.80")
DEFAULT_USER = "admin"
DEFAULT_PASS = "admin"


def init_cc(session: requests.Session, base_url: str) -> bool:
    """
    Initialize CC1110 2-wire debug interface.

    Args:
        session: Authenticated requests session.
        base_url: Flasher base HTTP URL.

    Returns:
        bool: True if debug interface responded with chip identification.
    """
    print(f"[*] Initializing CC debugger at {base_url}/cc_init...")
    try:
        resp = session.post(f"{base_url}/cc_init", timeout=10)
        resp.raise_for_status()
        text = resp.text.strip()
        print(f"[+] Response: {text}")
        if "0104" in text or "01" in text:
            print("[+] CC1110 detected!")
            return True
        else:
            print(f"[!] Warning: Unexpected response: {text}")
            return True
    except Exception as e:
        print(f"[-] Failed to init CC: {e}")
        return False


def upload_file(session: requests.Session, base_url: str, file_path: str) -> str:
    """
    Upload local binary file to ESP32 flasher filesystem.

    Args:
        session: Authenticated requests session.
        base_url: Flasher base HTTP URL.
        file_path: Local file path.

    Returns:
        str: Basename of the uploaded file on remote server.
    """
    filename = os.path.basename(file_path)
    remote_path = f"/{filename}"
    print(f"[*] Uploading {file_path} as {remote_path} to {base_url}/edit...")
    with open(file_path, "rb") as f:
        file_bytes = f.read()

    files = {
        'data': (remote_path, file_bytes, 'application/octet-stream')
    }
    resp = session.post(f"{base_url}/edit", files=files, timeout=30)
    resp.raise_for_status()
    print(f"[+] File uploaded successfully ({len(file_bytes)} bytes).")
    return filename


def flash_file(session: requests.Session, base_url: str, filename: str):
    """Trigger flash programming routine on the flasher."""
    print(f"[*] Triggering flash for {filename}...")
    resp = session.post(f"{base_url}/flash_cc", params={'file': filename}, timeout=10)
    resp.raise_for_status()
    print(f"[+] Flash command response: {resp.text.strip()}")


def poll_status(session: requests.Session, base_url: str, duration: int = 15):
    """Poll flasher logs during programming."""
    print("[*] Monitoring flash progress...")
    last_rx_log = ""
    start_time = time.time()

    while time.time() - start_time < duration:
        try:
            resp = session.get(f"{base_url}/get_status", params={'received_log': last_rx_log}, timeout=5)
            if resp.status_code == 200 and resp.text:
                status_raw = resp.text
                if status_raw.strip():
                    print(f"    [Status] {status_raw.strip()}")
                if "100" in status_raw or "done" in status_raw.lower() or "finish" in status_raw.lower():
                    break
        except Exception:
            pass
        time.sleep(0.5)


def reset_and_run(session: requests.Session, base_url: str):
    """Release CC1110 RESET line and exit debug mode."""
    print(f"[*] Resetting and running CC1110 ({base_url}/cc_exit_dbg)...")
    try:
        resp = session.post(f"{base_url}/cc_exit_dbg", timeout=10)
        print(f"[+] Reset response: {resp.text.strip()}")
    except Exception as e:
        print(f"[-] Reset failed: {e}")


def main():
    parser = argparse.ArgumentParser(
        description="Flash TI CC1110 via ESP_CC_Flasher",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    parser.add_argument("file", help="Path to .bin or .hex file to flash")
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"Host URL (default: {DEFAULT_HOST})")
    parser.add_argument("--user", default=DEFAULT_USER, help="HTTP Basic Auth user")
    parser.add_argument("--password", default=DEFAULT_PASS, help="HTTP Basic Auth password")
    parser.add_argument("--no-run", action="store_true", help="Do not exit debug / run chip after flash")

    args = parser.parse_args()

    if not os.path.exists(args.file):
        print(f"[-] File not found: {args.file}")
        sys.exit(1)

    session = requests.Session()
    session.auth = HTTPBasicAuth(args.user, args.password)

    if not init_cc(session, args.host):
        print("[-] Aborting flash.")
        sys.exit(1)

    filename = upload_file(session, args.host, args.file)
    flash_file(session, args.host, filename)
    poll_status(session, args.host, duration=10)

    if not args.no_run:
        reset_and_run(session, args.host)

    print("[+] Done!")


if __name__ == "__main__":
    main()
