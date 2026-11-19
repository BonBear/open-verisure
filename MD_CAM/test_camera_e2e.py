#!/usr/bin/env python3
"""
test_camera_e2e.py - End-to-end check of the MD CAM through the gateway (RF downlink, pictures,
alarm), driven over MQTT and observed on the gateway's USB serial log.

Needs: the camera powered and flashed, the gateway on USB (found by its USB ID, or --port) and on MQTT.
The camera ID is --device, or by default the DEVICE_ID in MD_CAM/firmware/config.h.
The broker address, credentials and base topic are read from 868Receiver/src/secrets.h and are
never printed. Lines on the gateway log that could show a key (32 hex digits) are masked.

    python MD_CAM/test_camera_e2e.py                 # poll, pictures, delta, alarm
    python MD_CAM/test_camera_e2e.py --gateway-restart   # also: base picture after a restart

Checks:
  1. the camera polls (the gateway answers with a downlink)
  2. picture/set D1 -> a full 320x240 JPEG with no missing chunks
  3. picture/set D3 right after -> a delta (queued so it goes out with the poll after the
     picture, within BF_KEEP_ON_S while the Blackfin still holds the reference)
  4. a poll follows right after a picture (the camera polls after every picture)
  5. alarm/set ON reaches the camera (armed 1), alarm/set OFF too (armed 0)
  6. the full picture came in 56-byte packets (type 0x82: 40 bytes each, so ~len/40 chunks)
  7. picture/set D4 -> a grayscale flash picture on MQTT, and a D3 after it is still a delta
     against the colour picture (the gateway keeps it as the base)
  8. picture/set D5 / D6 -> a picture (colour delta / 640x480 with light, grayscale flash
     picture in the dark; both are accepted, the log says which)
  9. (--gateway-restart) after "reboot" a delta without base makes the gateway ask for 0xD1,
     and a full picture arrives (needs BF_KEEP_ON_S longer than the reboot plus a poll,
     else the camera sends a full picture right away)
Exit code 0 when every check passed.
"""
import argparse
import os
import re
import sys
import threading
import time

import serial
import paho.mqtt.client as mqtt
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from port_selector import GATEWAY_USB, find_usb_port
import cc1110_tools
import derive_key

HERE = os.path.dirname(os.path.abspath(__file__))
SECRETS = os.path.join(HERE, "..", "868Receiver", "src", "secrets.h")
CONFIG_H = os.path.join(HERE, "firmware", "config.h")
HEX32 = re.compile(r"\b[0-9A-Fa-f]{32}\b")


def jpeg_components(j: bytes) -> int:
    """Number of colour components in the SOF0 frame header (1 = grayscale), 0 if not found."""
    i = 2
    while i + 9 < len(j):
        if j[i] != 0xFF:
            return 0
        marker, seglen = j[i + 1], (j[i + 2] << 8) | j[i + 3]
        if marker == 0xC0:
            return j[i + 9]
        i += 2 + seglen
    return 0


def secret(name: str, default: str = "") -> str:
    try:
        text = open(SECRETS, encoding="utf-8").read()
    except OSError:
        return default
    m = re.search(r'#define\s+%s\s+"([^"]*)"' % name, text)
    return m.group(1) if m else default


class GatewayLog:
    """Reads the gateway's serial port in the background; wait() finds new lines by regex."""

    def __init__(self, port: str):
        self.ser = serial.Serial(port, 115200, timeout=0.3)
        self.lines = []
        self.lock = threading.Lock()
        self.stop = False
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while not self.stop:
            try:
                raw = self.ser.readline()
            except serial.SerialException:
                time.sleep(1)       # the port goes away while the gateway reboots
                try:
                    self.ser.close()
                    self.ser.open()
                except serial.SerialException:
                    pass
                continue
            if raw:
                line = HEX32.sub("<hidden>", raw.decode("utf-8", "replace").rstrip())
                with self.lock:
                    self.lines.append((time.time(), line))

    def mark(self) -> int:
        with self.lock:
            return len(self.lines)

    def wait(self, pattern: str, timeout: float, since: int):
        """First line at index >= since that matches; returns (time, line, index) or None."""
        rx = re.compile(pattern)
        t0 = time.time()
        while time.time() - t0 < timeout:
            with self.lock:
                for i in range(since, len(self.lines)):
                    if rx.search(self.lines[i][1]):
                        return self.lines[i][0], self.lines[i][1], i
            time.sleep(0.2)
        return None

    def send(self, cmd: str):
        self.ser.write((cmd + "\n").encode())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--port", default=None,
                    help="gateway USB serial port (default: found by USB ID, e.g. COM9 or /dev/ttyACM0)")
    ap.add_argument("--device", default=None,
                    help="camera device ID (default: DEVICE_ID in MD_CAM/firmware/config.h)")
    ap.add_argument("--gateway-restart", action="store_true", help="also test the base picture after a restart")
    args = ap.parse_args()
    args.port = args.port or os.environ.get("RECEIVER_PORT") or find_usb_port(GATEWAY_USB)
    if not args.port:
        print("[-] Gateway not found on USB; pass --port")
        return 2
    try:
        args.device = derive_key.clean_device_id(args.device) if args.device \
            else cc1110_tools.config_device_id(CONFIG_H)
    except ValueError as e:
        print(f"[-] {e}")
        return 2
    if not args.device:
        print("[-] No camera ID: pass --device (no MD_CAM/firmware/config.h)")
        return 2

    base = secret("DEFAULT_MQTT_TOPIC", "verisure")
    dev = args.device
    results = []

    def check(name: str, ok: bool, detail: str):
        results.append(ok)
        print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)
        return ok

    log = GatewayLog(args.port)
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="md-cam-e2e")
    if secret("DEFAULT_MQTT_USER"):
        client.username_pw_set(secret("DEFAULT_MQTT_USER"), secret("DEFAULT_MQTT_PASS"))
    images = []                      # (time, payload) of pictures published by the gateway

    def on_message(cl, userdata, msg):
        if not msg.retain:
            images.append((time.time(), msg.payload))

    client.on_message = on_message
    client.connect(secret("DEFAULT_MQTT_HOST", "192.168.0.22"), 1883, 10)
    client.subscribe(f"{base}/{dev}/image")
    client.loop_start()

    def publish(topic: str, payload: str):
        client.publish(f"{base}/{topic}", payload).wait_for_publish(5)

    picture_rx = r"\[ImageReceiver\] (JPEG|Delta) from " + dev
    poll_rx = r"\[Downlink\] -> " + dev

    # 1. polls
    m = log.mark()
    r = log.wait(poll_rx, 75, m)
    check("poll", r is not None, r[1].split("] ", 1)[1] if r else "no downlink in 75 s")

    # 2-8. picture chain. The Blackfin stays on only BF_KEEP_ON_S (15 s) after a picture, and
    # a queued command goes out with the next poll answer, which follows right after a
    # picture. So each command is published as soon as the previous one has been delivered:
    # it then reaches the camera with the poll after that picture, while the reference for
    # deltas is still in the Blackfin's SDRAM.
    chain = ["D1", "D3", "D4", "D3", "D5", "D6"]
    m = log.mark()
    publish(f"{dev}/picture/set", chain[0])
    pics = []
    for i, cmd in enumerate(chain):
        d = log.wait(poll_rx + r".*cmd 0x" + cmd, 120, m)
        if not d:
            pics.append((cmd, None, 0))
            check(f"{cmd} delivered", False, "command not delivered in 120 s")
            break
        n_img = len(images)
        if i + 1 < len(chain):
            publish(f"{dev}/picture/set", chain[i + 1])
        r = log.wait(picture_rx, 120, d[2] + 1)
        time.sleep(1.0)
        new = [p for t, p in images[n_img:]]
        # the first picture published after this command (the next one may already follow)
        pics.append((cmd, r, jpeg_components(new[0]) if new else 0))
        m = r[2] + 1 if r else log.mark()

    def detail(r):
        return r[1].split("] ", 1)[1] if r else "no picture in 120 s"

    def is_delta(r):
        return bool(r and "Delta" in r[1] and "ignored" not in r[1] and "could not" not in r[1])

    def full_colour(r):
        return bool(r and "JPEG" in r[1] and "width 320" in r[1] and "id -1" not in r[1])

    # Each D3 may come as a full colour picture: the camera sends one when more than half
    # the picture changed (the exposure still moves right after a cold start or after the
    # flash frame). At least one of them must be a real delta.
    d3 = [r for cmd, r, c in pics[1:4:2]]

    for k, (cmd, r, comps) in enumerate(pics):
        if k == 0:
            ok = bool(r and "JPEG" in r[1] and "width 320" in r[1] and " 0/" in r[1])
            check("D1 full picture", ok, detail(r))
            mm = re.search(r"(\d+) bytes.* (\d+)/(\d+) chunks", r[1]) if r else None
            if mm:
                nbytes, chunks = int(mm.group(1)), int(mm.group(3))
                check("56-byte picture packets", chunks == (nbytes + 39) // 40,
                      f"{chunks} chunks for {nbytes} bytes (8-byte packets would be {(nbytes + 7) // 8})")
            if r:
                # the camera polls right after every picture
                p = log.wait(poll_rx, 15, r[2] + 1)
                check("poll after picture", p is not None and p[0] - r[0] < 10,
                      f"{p[0] - r[0]:.1f} s after the picture" if p else "no poll within 15 s")
        elif k == 1:
            check("D3 delta (or full colour)", is_delta(r) or full_colour(r), detail(r))
        elif k == 2:
            check("D4 flash picture", bool(r and "JPEG" in r[1] and comps == 1),
                  detail(r) + (f", {comps} component(s) on MQTT" if r else ""))
        elif k == 3:
            check("D3 after D4 (delta or full colour)", is_delta(r) or full_colour(r), detail(r))
            check("a delta among the D3s", any(is_delta(x) for x in d3),
                  ", ".join("delta" if is_delta(x) else "full" for x in d3))
        else:
            kind = "colour delta" if is_delta(r) else (
                "grayscale flash picture (dark)" if comps == 1 else "colour picture")
            check(f"{cmd} alarm picture", bool(r and "ignored" not in r[1] and "could not" not in r[1]),
                  (kind + ": " + detail(r)) if r else detail(r))

    # 5. alarm on / off through the downlink
    for state, want in (("ON", "armed 1"), ("OFF", "armed 0")):
        m = log.mark()
        t0 = time.time()
        publish("alarm/set", state)
        r = log.wait(poll_rx + ": " + want, 240, m)
        check(f"alarm {state}", r is not None, f"delivered after {r[0] - t0:.0f} s" if r else "not delivered in 240 s")

    # 6. base picture after a gateway restart (Blackfin still holds the reference)
    if args.gateway_restart:
        m = log.mark()
        publish(f"{dev}/picture/set", "D3")      # make sure the camera has a fresh reference
        log.wait(picture_rx, 120, m)
        log.send("reboot")
        time.sleep(3)
        log.wait(r"MQTT.*[Cc]onnected", 40, log.mark())
        m = log.mark()
        publish(f"{dev}/picture/set", "D3")
        q = log.wait(r"No base for this delta: full picture 0xD1 for " + dev + " queued", 120, m)
        j = log.wait(r"\[ImageReceiver\] JPEG from " + dev, 180, q[2] + 1) if q else None
        check("base after gateway restart", j is not None,
              ("0xD1 queued, full picture " + j[1].split("] ", 1)[1]) if j else
              ("0xD1 queued but no full picture" if q else "no base request (was the delta a full picture?)"))

    log.stop = True
    client.loop_stop()
    passed = sum(results)
    print(f"\n{passed}/{len(results)} checks passed")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
