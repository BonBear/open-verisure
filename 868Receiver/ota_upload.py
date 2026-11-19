#!/usr/bin/env python3
"""
ota_upload.py - Firmware update of the gateway over Wi-Fi (POST /api/ota).

Uploads the gateway's firmware.bin, waits for the reboot and checks /api/status: the gateway
must come back running exactly the uploaded image (its firmware MD5).

The web portal password is read from a file (--password-file) or the environment variable
GATEWAY_PASS. It is never taken from the command line and never printed.

Usage examples:
    # build, then update the gateway over Wi-Fi
    pio run -d 868Receiver
    GATEWAY_PASS=... python 868Receiver/ota_upload.py --host verisure-gateway.local

    # another image, password from a file, wait until the new firmware has confirmed itself
    python 868Receiver/ota_upload.py --bin path/to/firmware.bin --password-file ~/.gateway_pass --wait-valid

The image is checked locally first (an ESP32-S2 application image); the gateway checks it again
before it writes anything and verifies the whole image (and the MD5 sent along) before it boots
it. A new firmware that does not reach Wi-Fi within 5 minutes is rolled back by the gateway.
Plain HTTP: use only on a trusted LAN (see SECURITY.md).
"""
import argparse
import base64
import hashlib
import http.client
import json
import os
import socket
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ENV = "lolin_s2_mini"
ESP32S2_CHIP_ID = 2
IMAGE_MAGIC = 0xE9
APP_DESC_MAGIC = 0xABCD5432
HEADER_CHECK_LEN = 36


def check_app_header(head: bytes, chip_id: int = ESP32S2_CHIP_ID):
    """The gateway's otaCheckAppHeader(): None if 'head' starts an application image for
    chip_id, else the reason."""
    if len(head) < HEADER_CHECK_LEN:
        return "image too short"
    if head[0] != IMAGE_MAGIC:
        return "not an ESP32 firmware image (wrong magic byte)"
    if head[1] == 0 or head[1] > 16:
        return "not an ESP32 firmware image (bad segment count)"
    if int.from_bytes(head[12:14], "little") != chip_id:
        return "firmware image for another chip"
    if int.from_bytes(head[32:36], "little") != APP_DESC_MAGIC:
        return "not an application image (no app description)"
    return None


def read_password(password_file):
    """The portal password from the file (first line) or GATEWAY_PASS; None if neither."""
    if password_file:
        with open(os.path.expanduser(password_file), encoding="utf-8") as f:
            pw = f.readline().rstrip("\r\n")
        return pw or None
    return os.environ.get("GATEWAY_PASS") or None


def auth_header(password: str) -> str:
    return "Basic " + base64.b64encode(("admin:" + password).encode("utf-8")).decode("ascii")


class _Progress:
    """File wrapper for http.client that prints the upload progress."""

    def __init__(self, f, total):
        self.f, self.total, self.sent, self.last = f, total, 0, -1

    def read(self, n=-1):
        data = self.f.read(65536 if n is None or n < 0 else n)
        self.sent += len(data)
        pct = 100 * self.sent // self.total if self.total else 100
        if pct // 10 != self.last:
            self.last = pct // 10
            print(f"\r    uploading {pct:3d} %", end="", flush=True)
        return data


def request(host, method, path, password, body=None, headers=None, timeout=10):
    """One HTTP request; returns (status, parsed JSON or text). Host header = the name given."""
    h = {"Host": host, "Authorization": auth_header(password)}
    h.update(headers or {})
    conn = http.client.HTTPConnection(host, 80, timeout=timeout)
    try:
        conn.request(method, path, body=body, headers=h)
        r = conn.getresponse()
        text = r.read().decode("utf-8", "replace")
    finally:
        conn.close()
    try:
        return r.status, json.loads(text)
    except ValueError:
        return r.status, text


def get_status(host, password, timeout=5):
    status, reply = request(host, "GET", "/api/status", password, timeout=timeout)
    if status != 200 or not isinstance(reply, dict):
        raise RuntimeError(f"/api/status answered HTTP {status}: {reply}")
    return reply


def main(argv=None):
    ap = argparse.ArgumentParser(description="Update the gateway firmware over Wi-Fi.")
    ap.add_argument("--host", default="verisure-gateway.local",
                    help="gateway name or IP (default verisure-gateway.local)")
    ap.add_argument("--bin", default=os.path.join(HERE, ".pio", "build", DEFAULT_ENV, "firmware.bin"),
                    help="firmware image (default .pio/build/%s/firmware.bin)" % DEFAULT_ENV)
    ap.add_argument("--password-file", help="file whose first line is the web portal password "
                    "(else the environment variable GATEWAY_PASS)")
    ap.add_argument("--force", action="store_true", help="upload even if the gateway already runs this image")
    ap.add_argument("--wait-valid", action="store_true",
                    help="also wait until the new firmware has confirmed itself (Wi-Fi + 30 s)")
    ap.add_argument("--timeout", type=int, default=120, help="seconds to wait for the reboot (default 120)")
    args = ap.parse_args(argv)

    password = read_password(args.password_file)
    if not password:
        print("[!] No portal password: use --password-file or set GATEWAY_PASS.")
        return 2

    with open(args.bin, "rb") as f:
        image = f.read()
    why = check_app_header(image[:HEADER_CHECK_LEN])
    if why:
        print(f"[!] {args.bin}: {why}")
        return 2
    md5 = hashlib.md5(image).hexdigest()
    print(f"[*] Image {args.bin}: {len(image)} bytes, MD5 {md5}")

    try:
        before = get_status(args.host, password)
    except (OSError, RuntimeError, http.client.HTTPException) as e:
        print(f"[!] Gateway {args.host} not reachable or refused: {e}")
        return 1
    fw = before.get("firmware", {})
    print(f"[*] Gateway runs slot {fw.get('partition', '?')} ({fw.get('state', '?')}), MD5 {fw.get('md5', '?')}")
    if fw.get("md5") == md5 and not args.force:
        print("[=] The gateway already runs this image (--force uploads it anyway).")
        return 0

    t0 = time.time()
    with open(args.bin, "rb") as f:
        try:
            status, reply = request(args.host, "POST", "/api/ota", password, body=_Progress(f, len(image)),
                                    headers={"Content-Type": "application/octet-stream",
                                             "Content-Length": str(len(image)),
                                             "X-Gateway": "1", "X-OTA-MD5": md5},
                                    timeout=60)
        except (OSError, http.client.HTTPException) as e:
            # a refused upload is answered at once and its connection closed by the gateway
            print(f"\n[!] Upload refused or connection lost: {e} (the running firmware stays; check the "
                  "password, and that the gateway is not in its setup access point)")
            return 1
    print(f"\n[*] Upload took {time.time() - t0:.1f} s: HTTP {status} {reply}")
    if status != 200 or not isinstance(reply, dict) or not reply.get("ok"):
        print("[!] Update refused; the gateway keeps its running firmware.")
        return 1

    print("[*] Waiting for the reboot...")
    time.sleep(5)
    deadline = time.time() + args.timeout
    after = None
    while time.time() < deadline:
        try:
            after = get_status(args.host, password)
            if after.get("firmware", {}).get("md5") == md5:
                break
        except (OSError, RuntimeError, socket.timeout, http.client.HTTPException):
            pass   # rebooting: connection refused or cut mid-response
        time.sleep(3)
    else:
        print(f"[!] The gateway did not come back with the new image within {args.timeout} s.")
        return 1

    if args.wait_valid:
        while time.time() < deadline + 120 and after["firmware"].get("state") == "pending_verify":
            time.sleep(5)
            try:
                after = get_status(args.host, password)
            except (OSError, RuntimeError):
                pass
    fw = after["firmware"]
    mqtt = after.get("mqtt", {})
    print(f"[+] Gateway runs the new image: slot {fw.get('partition')} ({fw.get('state')}), MD5 {fw.get('md5')}")
    print(f"    Wi-Fi {after.get('wifi', {}).get('ip')} | MQTT {'connected' if mqtt.get('connected') else mqtt.get('status')}"
          f" | packets {after.get('radio', {}).get('total_packets')}")
    if args.wait_valid and fw.get("state") != "valid":
        print("[!] The new firmware has not confirmed itself (it is rolled back if it does not reach Wi-Fi).")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
