# Home Assistant & MQTT Integration

This document details how the gateway integrates Verisure Sub-1GHz sensors into **Home Assistant** via MQTT Auto-Discovery, as well as the Gateway Web Dashboard's REST API.

---

## 📡 MQTT Topic Architecture

```
verisure/
  status                           <-- LWT (Birth: "online", Last Will: "offline")
  <deviceId>/
    state                          <-- JSON telemetry published on each event
homeassistant/
  binary_sensor/
    verisure_<deviceId>/
      contact/config               <-- Auto-Discovery for Contact (door open/closed)
      vibration/config             <-- Auto-Discovery for Vibration (shock)
      button/config                <-- Auto-Discovery for Tamper (casing cover)
  sensor/
    verisure_<deviceId>/
      battery/config               <-- Auto-Discovery for Battery (%)
      voltage/config               <-- Auto-Discovery for Battery Voltage (mV)
      rssi/config                  <-- Auto-Discovery for Signal Strength (dBm)
```

---

## 🎯 Entity Configuration in Home Assistant

Each sensor creates a dedicated device entity in Home Assistant exposing 6 entities:

| Entity | Platform | Device Class (`device_class`) | Unit | Payload / Template | Description |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **Contact** | `binary_sensor` | `door` | – | `{{ value_json.contact }}`<br>(`OPEN` / `CLOSED`) | Door/window contact (renders door icon with Open/Closed state) |
| **Vibration** | `binary_sensor` | `vibration` | – | `{{ value_json.vibration }}`<br>(`ON` / `OFF`) | Shock and vibration detection |
| **Tamper** | `binary_sensor` | `tamper` | – | `{{ value_json.button }}`<br>(`ON` / `OFF`) | Tamper switch (reports Tamper/Open if cover is removed) |
| **Battery** | `sensor` | `battery` | `%` | `{{ value_json.battery_pct }}` | Battery percentage (0–100%) |
| **Battery Voltage** | `sensor` | `voltage` | `mV` | `{{ value_json.battery_mv }}` | Accurate battery terminal voltage in millivolts |
| **Signal Strength** | `sensor` | `signal_strength` | `dBm` | `{{ value_json.rssi }}` | RF reception signal strength from sensor to gateway |

### Example Telemetry JSON (`verisure/2JHL2MVN/state`)
```json
{
  "contact": "CLOSED",
  "vibration": "OFF",
  "button": "OFF",
  "power_on": "OFF",
  "battery_pct": 100,
  "battery_mv": 3370,
  "low_battery": "OFF",
  "rssi": -49,
  "lqi": 127,
  "seq": 28
}
```

---

## 🌐 Gateway Web Dashboard & REST API

The gateway hosts an embedded web server on port 80 ([http://192.168.0.142](http://192.168.0.142)). It provides a dark-mode responsive interface for monitoring sensor telemetry and configuring network parameters.

### REST API Endpoints:
* **`GET /api/status`:** Returns real-time JSON status for Wi-Fi, radio noise floor, MQTT broker link, and all registered sensors.
* **`GET /api/scan`:** Performs an asynchronous Wi-Fi scan and returns available networks with RSSI and encryption flags.
* **`POST /api/save`:** Persists Wi-Fi SSID, password, MQTT broker host, port, credentials, topic prefix, and Master Key into ESP32-S2 NVS (Non-Volatile Storage).
* **`POST /api/reboot`:** Triggers a software restart of the gateway.
* **`POST /api/reset`:** Resets NVS parameters to compiled defaults.
