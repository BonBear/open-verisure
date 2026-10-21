# Home Assistant & MQTT Integration

Detta dokument beskriver hur gatewayen automatiskt integrerar Verisure-sensorerna i **Home Assistant** via MQTT Auto-Discovery, samt webbdashboardens REST API.

---

## 📡 MQTT Topic-Arkitektur

```
verisure/
  status                           <-- LWT (Birth: "online", Last Will: "offline")
  <deviceId>/
    state                          <-- JSON-telemetri vid varje sändning
homeassistant/
  binary_sensor/
    verisure_<deviceId>/
      contact/config               <-- Auto-Discovery för Contact (öppning/stängning)
      vibration/config             <-- Auto-Discovery för Vibration (skak)
      button/config                <-- Auto-Discovery för Tamper (sabotagelock)
  sensor/
    verisure_<deviceId>/
      battery/config               <-- Auto-Discovery för Batteri (%)
      voltage/config               <-- Auto-Discovery för Batterispänning (mV)
      rssi/config                  <-- Auto-Discovery för Signalstyrka (dBm)
```

---

## 🎯 Entitetskonfiguration i Home Assistant

Varje sensor skapar en egen sammanhållen enhet (*Device*) i Home Assistant med följande 6 entiteter:

| Entitet | Typ | Enhetsklass (`device_class`) | Enhet | Payload / Mall | Beskrivning |
| :--- | :---: | :---: | :---: | :--- | :--- |
| **Contact** | `binary_sensor` | `door` | – | `{{ value_json.contact }}`<br>(`OPEN` / `CLOSED`) | Dörrkontakt (visar dörrikon och "Öppen" / "Stängd" i HA) |
| **Vibration** | `binary_sensor` | `vibration` | – | `{{ value_json.vibration }}`<br>(`ON` / `OFF`) | Skak/vibrationsdetektering |
| **Tamper** | `binary_sensor` | `tamper` | – | `{{ value_json.button }}`<br>(`ON` / `OFF`) | Sabotagekontakt (visar "Sabotage / Öppen" vid lock av) |
| **Batteri** | `sensor` | `battery` | `%` | `{{ value_json.battery_pct }}` | Batterinivå i procent |
| **Batterispänning**| `sensor` | `voltage` | `mV` | `{{ value_json.battery_mv }}` | Exakt batterispänning i millivolt (t.ex. `3370 mV`) |
| **Signalstyrka** | `sensor` | `signal_strength` | `dBm` | `{{ value_json.rssi }}` | RF-mottagningsstyrka från sensorn till gatewayen |

### Exempel på Telemetri-JSON (`verisure/2JHL2MVN/state`)
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

Gatewayen har en inbyggd webbserver på port 80 ([http://192.168.0.142](http://192.168.0.142)). Den tillhandahåller ett mörkt och responsivt gränssnitt för att övervaka sensorer och konfigurera nätverket.

### REST API Endpoints:
* **`GET /api/status`:** Returnerar JSON med aktuell status för Wi-Fi, radiobrus, MQTT-anslutning och alla registrerade sensorer i realtid.
* **`GET /api/scan`:** Utför en asynkron Wi-Fi-skanning och returnerar en lista över tillgängliga SSID med RSSI och krypteringsstatus.
* **`POST /api/save`:** Sparar Wi-Fi SSID, lösenord, MQTT broker, port, användare, ämne och Master Key i ESP32-S2:ans permanenta NVS (Non-Volatile Storage).
* **`POST /api/reboot`:** Startar om gatewayen.
* **`POST /api/reset`:** Fabriksåterställer NVS-konfigurationen.
