#!/usr/bin/env python3
"""
dump_cc1110.py - Safely dump the 32 KB Flash of an unlocked CC1110F32 via ESP_CC_Flasher.
NEVER calls /erase_cc or /flash_cc!
"""
import os
import re
import sys
import time
import requests
from requests.auth import HTTPBasicAuth

CC_HOST = os.environ.get("CC_HOST", "http://cc.local")
CC_USER = "admin"
CC_PASS = "admin"
OUT_DIR = os.path.dirname(os.path.abspath(__file__))
REMOTE_DUMP_NAME = "md_cam_orig.bin"
LOCAL_DUMP_PATH = os.path.join(OUT_DIR, REMOTE_DUMP_NAME)


def get_latest_log_id(session: requests.Session, host: str) -> int:
    try:
        r = session.get(f"{host}/get_status", timeout=5)
        ids = [int(x) for x in re.findall(r'\{"(\d+)":', r.text)]
        return max(ids) if ids else 0
    except Exception:
        return 0


def main():
    session = requests.Session()
    session.auth = HTTPBasicAuth(CC_USER, CC_PASS)

    # 1. Check /cc_init (Safe read-only chip ID & status check)
    print(f"[*] Calling POST {CC_HOST}/cc_init ...")
    r_init = session.post(f"{CC_HOST}/cc_init", timeout=5)
    init_text = r_init.text.strip()
    print(f"[+] Response: {init_text}")

    if "0104" not in init_text:
        print("[-] Expected CC1110F32 (0104). Aborting dump.")
        sys.exit(1)

    last_log_id = get_latest_log_id(session, CC_HOST)
    print(f"[*] Baseline log ID: {last_log_id}")

    # 2. Start flash dump (32768 bytes = 32 KB)
    dump_size = 32768
    print(f"[*] Starting 32 KB flash dump to /{REMOTE_DUMP_NAME} ({dump_size} bytes)...")
    r_dump = session.post(
        f"{CC_HOST}/dump_cc",
        params={"file": REMOTE_DUMP_NAME, "size": str(dump_size)},
        timeout=15,
    )
    print(f"[+] Dump command response: {r_dump.text.strip()}")

    # 3. Poll status until complete
    t0 = time.time()
    dump_done = False
    last_pct = -1
    while time.time() - t0 < 120:
        try:
            r = session.get(f"{CC_HOST}/get_status", params={"received_log": last_log_id}, timeout=5)
            if r.status_code == 200 and r.text.strip():
                for m in re.finditer(r'\{"(\d+)":\s*"(\d+)":\s*"([^"]*)"\}', r.text):
                    lid = int(m.group(1))
                    chan = int(m.group(2))
                    msg = m.group(3)
                    if lid > last_log_id:
                        last_log_id = lid
                        if chan == 1:
                            pct = int(msg) if msg.isdigit() else -1
                            if pct != last_pct and pct % 10 == 0:
                                print(f"    Progress: {pct}%")
                                last_pct = pct
                        else:
                            print(f"    [{lid}] (ch {chan}) {msg}")
                            if "done" in msg.lower() or "finish" in msg.lower() or "dumping flash to file" in msg.lower():
                                if "done" in msg.lower() or "finish" in msg.lower() or "took" in msg.lower():
                                    dump_done = True
                if dump_done or last_pct >= 100:
                    # Wait briefly for file flush on ESP32 SPIFFS
                    time.sleep(1.5)
                    break
        except Exception as e:
            print(f"    [!] Poll warning: {e}")
        time.sleep(0.5)

    # 4. Download the dumped file from ESP32 SPIFFS
    print(f"[*] Downloading {CC_HOST}/{REMOTE_DUMP_NAME} ...")
    for url in [f"{CC_HOST}/{REMOTE_DUMP_NAME}", f"{CC_HOST}/edit?download=/{REMOTE_DUMP_NAME}"]:
        r_dl = session.get(url, timeout=30)
        if r_dl.status_code == 200 and len(r_dl.content) > 0 and b"FileNotFound" not in r_dl.content:
            with open(LOCAL_DUMP_PATH, "wb") as f:
                f.write(r_dl.content)
            print(f"[+] Saved {len(r_dl.content)} bytes to {LOCAL_DUMP_PATH}!")
            break
    else:
        print(f"[-] Could not download /{REMOTE_DUMP_NAME} directly (status {r_dl.status_code}). Checking /list...")
        r_list = session.get(f"{CC_HOST}/list?dir=/", timeout=10)
        print(f"[+] SPIFFS files: {r_list.text}")


if __name__ == "__main__":
    main()
