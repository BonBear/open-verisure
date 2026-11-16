#!/usr/bin/env python3
"""
test_resend.py - Test the resend of missing picture chunks. The gateway drops every n-th
picture chunk ("drop n" on its serial console), then pictures are requested over MQTT; the
gateway log shows the resend rounds, and the last published picture of each request is checked
for zero-filled holes. Credentials are read from 868Receiver/src/secrets.h and never printed.

    python MD_CAM/test_resend.py 7 D1 D2      # drop every 7th chunk, request D1 and D2

Needs the camera powered and the gateway on COM9. "drop 0" is sent at the end.
"""
import os
import re, sys, time, threading
import serial
import paho.mqtt.client as mqtt
SECRETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "868Receiver", "src", "secrets.h")
txt = open(SECRETS, encoding="utf-8").read()
sec = lambda n, d="": (re.search(r'#define\s+%s\s+"([^"]*)"' % n, txt) or [None, d])[1]
lines, images = [], []
ser = serial.Serial("COM9", 115200, timeout=0.3)
def rd():
    while True:
        l = ser.readline()
        if l:
            lines.append((time.time(), re.sub(r"\b[0-9A-Fa-f]{32}\b", "<hidden>", l.decode("utf-8", "replace").rstrip())))
threading.Thread(target=rd, daemon=True).start()
c = mqtt.Client(client_id="claude-rs-%d" % os.getpid())
if sec("DEFAULT_MQTT_USER"):
    c.username_pw_set(sec("DEFAULT_MQTT_USER"), sec("DEFAULT_MQTT_PASS"))
c.on_message = lambda cl, u, m: None if m.retain else images.append((time.time(), m.payload))
c.connect(sec("DEFAULT_MQTT_HOST", "192.168.0.22"), 1883, 10)
c.subscribe("verisure/2NCEKS3T/image"); c.loop_start()
def wait(rx, t, since):
    r = re.compile(rx); t0 = time.time()
    while time.time() - t0 < t:
        for i in range(since, len(lines)):
            if r.search(lines[i][1]):
                return i
        time.sleep(0.1)
    return None
n = sys.argv[1]
ser.write(f"drop {n}\n".encode()); time.sleep(1)
for cmd in sys.argv[2:]:
    m = len(lines); k = len(images)
    c.publish("verisure/2NCEKS3T/picture/set", cmd)
    p = wait(r"ImageReceiver\] JPEG from", 120, m)
    end = wait(r"complete after resend|Resend for .* (given up|stopped)", 40, p or m)
    time.sleep(2)
    for t, l in lines[m:]:
        if re.search(r"ImageReceiver|resend", l):
            print("   ", l)
    if len(images) > k:
        b = images[-1][1]
        holes = len(re.findall(rb"\x00{16,}", b))
        print(f"{cmd}: {len(images) - k} picture(s) published, last {len(b)} bytes, EOI {b.endswith(bytes([255, 217]))}, zero holes {holes}")
    print()
ser.write(b"drop 0\n"); time.sleep(1)
c.loop_stop()
