#!/usr/bin/env python3
"""
test_camera_e2e.py - End-to-end check of the MD CAM through the gateway (RF downlink, pictures,
alarm), driven over MQTT and observed on the gateway's USB serial log.

Needs: the camera powered and flashed, the gateway on USB (default COM9) and on MQTT.
The broker address, credentials and base topic are read from 868Receiver/src/secrets.h and are
never printed. Lines on the gateway log that could show a key (32 hex digits) are masked.

    python MD_CAM/test_camera_e2e.py                 # poll, pictures, delta, alarm
    python MD_CAM/test_camera_e2e.py --gateway-restart   # also: base picture after a restart

Checks:
  1. the camera polls (the gateway answers with a downlink)
  2. picture/set D1 -> a full 320x240 JPEG with no missing chunks
  3. picture/set D3 right after -> a delta (the Blackfin is kept powered between pictures)
  4. a poll follows right after a picture (the camera polls after every picture)
  5. alarm/set ON reaches the camera (armed 1), alarm/set OFF too (armed 0)
  6. the full picture came in 56-byte packets (type 0x82: 40 bytes each, so ~len/40 chunks)
  7. picture/set D4 -> a grayscale flash picture on MQTT, and a D3 after it is still a delta
     against the colour picture (the gateway keeps it as the base)
  8. picture/set D5 / D6 -> a picture (colour delta / 640x480 with light, grayscale flash
     picture in the dark; both are accepted, the log says which)
  9. (--gateway-restart) after "reboot" a delta without base makes the gateway ask for 0xD1,
     and a full picture arrives
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

HERE = os.path.dirname(os.path.abspath(__file__))
SECRETS = os.path.join(HERE, "..", "868Receiver", "src", "secrets.h")
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
    ap.add_argument("--port", default="COM9", help="gateway USB serial port")
    ap.add_argument("--device", default="2NCEKS3T", help="camera device id")
    ap.add_argument("--gateway-restart", action="store_true", help="also test the base picture after a restart")
    args = ap.parse_args()

    base = secret("DEFAULT_MQTT_TOPIC", "verisure")
    dev = args.device
    results = []

    def check(name: str, ok: bool, detail: str):
        results.append(ok)
        print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)
        return ok

    log = GatewayLog(args.port)
    client = mqtt.Client(client_id="md-cam-e2e")
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

    # 2. full picture on request
    m = log.mark()
    publish(f"{dev}/picture/set", "D1")
    r = log.wait(picture_rx, 180, m)
    ok = bool(r and "JPEG" in r[1] and "width 320" in r[1] and " 0/" in r[1])
    check("D1 full picture", ok, r[1].split("] ", 1)[1] if r else "no picture in 180 s")
    if r:
        mm = re.search(r"(\d+) bytes.* (\d+)/(\d+) chunks", r[1])
        if mm:
            nbytes, chunks = int(mm.group(1)), int(mm.group(3))
            check("56-byte picture packets", chunks == (nbytes + 39) // 40,
                  f"{chunks} chunks for {nbytes} bytes (8-byte packets would be {(nbytes + 7) // 8})")

    # 4. poll right after the picture
    if r:
        # seen 0..5 s; before the fix it came only with the regular 30 s poll (or minutes later)
        p = log.wait(poll_rx, 15, r[2] + 1)
        check("poll after picture", p is not None and p[0] - r[0] < 10,
              f"{p[0] - r[0]:.1f} s after the picture" if p else "no poll within 15 s")

    # 3. delta while the Blackfin is still powered
    m = log.mark()
    publish(f"{dev}/picture/set", "D3")
    r = log.wait(picture_rx, 120, m)
    check("D3 delta", bool(r and "Delta" in r[1] and "ignored" not in r[1] and "could not" not in r[1]),
          r[1].split("] ", 1)[1] if r else "no picture in 120 s")

    # 7. grayscale flash picture, then a delta against the colour base
    m = log.mark()
    n_img = len(images)
    publish(f"{dev}/picture/set", "D4")
    r = log.wait(picture_rx, 120, m)
    time.sleep(2)
    gray = [p for t, p in images[n_img:]]
    comps = jpeg_components(gray[-1]) if gray else 0
    check("D4 flash picture", bool(r and "JPEG" in r[1] and comps == 1),
          (r[1].split("] ", 1)[1] + f", {comps} component(s) on MQTT") if r else "no picture in 120 s")
    m = log.mark()
    publish(f"{dev}/picture/set", "D3")
    r = log.wait(picture_rx, 120, m)
    # a delta against the colour base, or a full colour picture if the camera found more than
    # half the picture changed (the exposure can move after the flash frame); never "ignored"
    ok = bool(r and (("Delta" in r[1] and "ignored" not in r[1] and "could not" not in r[1]) or
                     ("JPEG" in r[1] and "width 320" in r[1] and "id -1" not in r[1])))
    check("D3 after D4", ok, r[1].split("] ", 1)[1] if r else "no picture in 120 s")

    # 8. alarm pictures: colour with light, flash picture in the dark
    for cmd in ("D5", "D6"):
        m = log.mark()
        n_img = len(images)
        publish(f"{dev}/picture/set", cmd)
        r = log.wait(picture_rx, 150, m)
        time.sleep(2)
        new = [p for t, p in images[n_img:]]
        kind = ""
        if r and "Delta" in r[1]:
            kind = "colour delta"
        elif new:
            kind = "grayscale flash picture (dark)" if jpeg_components(new[-1]) == 1 else "colour picture"
        check(f"{cmd} alarm picture", bool(r and "ignored" not in r[1] and "could not" not in r[1]),
              (kind + ": " + r[1].split("] ", 1)[1]) if r else "no picture in 150 s")

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
