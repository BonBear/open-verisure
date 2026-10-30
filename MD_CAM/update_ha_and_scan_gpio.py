#!/usr/bin/env python3
"""
update_ha_and_scan_gpio.py
1. Updates Home Assistant MQTT Discovery for 2NCEKS3T so it appears as
   "Verisure Motion Camera 2NCEKS3T" (GW MD CAM2) with a Motion entity (instead of Door/Vibration),
   and removes the accidental 2NCEKT3X test device from Home Assistant.
2. Opens COM9 and triggers a clean reset on CC1110 (via http://cc.local) to read the live
   GPIO Pull-Up / Pull-Down scan bytes (P0_PU, P0_PD, P1_PU, P2) from 2NCEKS3T.
"""
import os
import re
import json
import time
import socket
import struct
import serial
import requests
from requests.auth import HTTPBasicAuth

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.dirname(BASE_DIR)
SECRETS_H = os.path.join(REPO_DIR, "868Receiver", "src", "secrets.h")
GATEWAY_HTTP = "http://192.168.0.142"
CC_HOST = "http://cc.local"
DEVICE_ID = "2NCEKS3T"
STALE_ID = "2NCEKT3X"


def encode_varlen(length: int) -> bytes:
    out = bytearray()
    while True:
        digit = length % 128
        length //= 128
        if length > 0:
            digit |= 0x80
        out.append(digit)
        if length == 0:
            break
    return bytes(out)


def encode_utf8(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack("!H", len(b)) + b


def mqtt_publish_batch(host: str, port: int, user: str, password: str, messages: list[tuple[str, str, bool]]) -> None:
    sock = socket.create_connection((host, port), timeout=5.0)
    try:
        # Build CONNECT packet (MQTT 3.1.1)
        var_hdr = encode_utf8("MQTT") + bytes([4])  # Protocol Level 4
        flags = 0x02  # Clean Session
        payload = encode_utf8("agy-mdcam-cfg")
        if user:
            flags |= 0x80
            payload += encode_utf8(user)
            if password:
                flags |= 0x40
                payload += encode_utf8(password)
        var_hdr += bytes([flags]) + struct.pack("!H", 30)
        rem = var_hdr + payload
        sock.sendall(bytes([0x10]) + encode_varlen(len(rem)) + rem)
        connack = sock.recv(4)
        if len(connack) < 4 or connack[3] != 0:
            raise RuntimeError(f"MQTT CONNACK failed: {connack.hex()}")

        for topic, msg, retain in messages:
            hdr_byte = 0x30 | (0x01 if retain else 0x00)
            body = encode_utf8(topic) + msg.encode("utf-8")
            sock.sendall(bytes([hdr_byte]) + encode_varlen(len(body)) + body)
            time.sleep(0.02)

        # DISCONNECT
        sock.sendall(bytes([0xE0, 0x00]))
    finally:
        sock.close()


def get_mqtt_pass_from_secrets() -> str:
    if not os.path.exists(SECRETS_H):
        return ""
    with open(SECRETS_H, "r", encoding="utf-8", errors="ignore") as f:
        text = f.read()
    for pat in [r'#define\s+MQTT_PASS(?:WORD)?\s+"([^"]*)"', r'#define\s+DEFAULT_MQTT_PASS\s+"([^"]*)"']:
        m = re.search(pat, text)
        if m:
            return m.group(1)
    return ""


def update_home_assistant_discovery() -> None:
    r = requests.get(f"{GATEWAY_HTTP}/api/status", timeout=5)
    r.raise_for_status()
    cfg = r.json().get("config", {})
    host = cfg.get("mqtt_host", "192.168.0.22")
    port = int(cfg.get("mqtt_port", 1883))
    user = cfg.get("mqtt_user", "")
    base_topic = cfg.get("mqtt_topic", "verisure")
    password = get_mqtt_pass_from_secrets()

    msgs: list[tuple[str, str, bool]] = []

    # 1. Remove accidental 2NCEKT3X entities from Home Assistant
    for k in ["contact", "vibration", "motion", "button"]:
        msgs.append((f"homeassistant/binary_sensor/verisure_{STALE_ID}_{k}/config", "", True))
    for k in ["battery", "voltage", "rssi"]:
        msgs.append((f"homeassistant/sensor/verisure_{STALE_ID}_{k}/config", "", True))

    # 2. Remove Door (contact) and Vibration entities from 2NCEKS3T
    msgs.append((f"homeassistant/binary_sensor/verisure_{DEVICE_ID}_contact/config", "", True))
    msgs.append((f"homeassistant/binary_sensor/verisure_{DEVICE_ID}_vibration/config", "", True))

    # 3. Publish Motion Camera entities for 2NCEKS3T
    dev_block = {
        "identifiers": [f"verisure_{DEVICE_ID}"],
        "name": f"Verisure Motion Camera {DEVICE_ID}",
        "model": "GW MD CAM2 (Motion Detector & Camera)",
        "manufacturer": "Verisure",
        "sw_version": "2.0-CC1110",
        "via_device": "verisure_gateway",
    }
    state_topic = f"{base_topic}/{DEVICE_ID}/state"
    avail_topic = f"{base_topic}/status"

    entities = [
        ("binary_sensor", "motion", {
            "name": "Motion",
            "device_class": "motion",
            "value_template": "{{ value_json.contact }}",
            "payload_on": "OPEN",
            "payload_off": "CLOSED",
        }),
        ("binary_sensor", "button", {
            "name": "Tamper",
            "device_class": "tamper",
            "value_template": "{{ value_json.button }}",
            "payload_on": "ON",
            "payload_off": "OFF",
            "entity_category": "diagnostic",
        }),
        ("sensor", "battery", {
            "name": "Battery",
            "device_class": "battery",
            "state_class": "measurement",
            "unit_of_measurement": "%",
            "value_template": "{{ value_json.battery_pct }}",
            "entity_category": "diagnostic",
        }),
        ("sensor", "voltage", {
            "name": "Battery Voltage",
            "device_class": "voltage",
            "state_class": "measurement",
            "unit_of_measurement": "mV",
            "value_template": "{{ value_json.battery_mv }}",
            "entity_category": "diagnostic",
        }),
        ("sensor", "rssi", {
            "name": "Signal Strength",
            "device_class": "signal_strength",
            "state_class": "measurement",
            "unit_of_measurement": "dBm",
            "value_template": "{{ value_json.rssi }}",
            "entity_category": "diagnostic",
        }),
    ]

    for comp, key, fields in entities:
        payload_dict = {
            "unique_id": f"verisure_{DEVICE_ID}_{key}",
            "state_topic": state_topic,
            "availability_topic": avail_topic,
            "payload_available": "online",
            "payload_not_available": "offline",
            "device": dev_block,
        }
        payload_dict.update(fields)
        topic = f"homeassistant/{comp}/verisure_{DEVICE_ID}_{key}/config"
        msgs.append((topic, json.dumps(payload_dict, separators=(",", ":")), True))

    mqtt_publish_batch(host, port, user, password, msgs)
    print(f"[+] Updated Home Assistant MQTT Discovery on {host}:{port}:")
    print(f"    - Removed stale device {STALE_ID}")
    print(f"    - Configured {DEVICE_ID} as 'Verisure Motion Camera {DEVICE_ID}' (GW MD CAM2) with Motion sensor!")


def capture_gpio_from_com9() -> None:
    print("[*] Opening COM9 to capture live GPIO scan packet from 2NCEKS3T...")
    ser = serial.Serial("COM9", 115200, timeout=0.2)
    try:
        ser.reset_input_buffer()
        # Trigger a quick reset of CC1110 via cc.local so it immediately transmits Seq #0..2
        s = requests.Session()
        s.auth = HTTPBasicAuth("admin", "admin")
        s.post(f"{CC_HOST}/cc_init", timeout=5)
        time.sleep(0.1)
        s.post(f"{CC_HOST}/cc_exit_dbg", timeout=5)
        print("[+] Triggered CC1110 reboot via cc.local; listening on COM9 for 4 seconds...")

        t_end = time.time() + 4.0
        while time.time() < t_end:
            line = ser.readline()
            if line:
                txt = line.decode("utf-8", errors="replace").rstrip()
                print("   ", txt)
    finally:
        ser.close()


if __name__ == "__main__":
    update_home_assistant_discovery()
    capture_gpio_from_com9()
