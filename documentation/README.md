# Verisure Sub-1GHz & Home Assistant Gateway – Projektdokumentation

Välkommen till den officiella tekniska dokumentationen för reverse-engineering och integration av **Verisure CC1110 dörr- och fönstersensorer (868.950 MHz)** till **Home Assistant** via en specialbyggd **ESP32-S2 Gateway** med MQTT Auto-Discovery.

---

## 📁 Dokumentationsöversikt

| Dokument | Beskrivning |
| :--- | :--- |
| [**1. Gateway Hårdvara & Kabeldragning**](hardware_gateway_esp32s2.md) | LOLIN S2 Mini (ESP32-S2) till CC1101 SPI, pinout, ADC1 vs ADC2 interferensanalys och kopplingsdiagram. |
| [**2. CC-Flasher & Programmering**](hardware_cc_flasher.md) | ESP32 CC-Flasher till Verisure J2-header, pinout, felsökning och automatiserad flashning via HTTP API. |
| [**3. Verisure Sensorkort (CC1110F32)**](verisure_sensor_hardware.md) | Komplett hårdvarukartläggning: Reed-kontakt, skaksensor, sabotagekontakt (P0_1), LED:ar, 27 MHz kristall och SAW-filter. |
| [**4. Protokoll, RF & Kryptografi**](cryptography_and_protocol.md) | 868.950 MHz 2-FSK, registervärden (FSCAL0/PA_TABLE0), AES-128 KDF nyckelhärledning, paketstruktur och replay-skydd. |
| [**5. Home Assistant & MQTT Integration**](homeassistant_mqtt.md) | MQTT Auto-Discovery arkitektur, entiteter (Contact, Vibration, Tamper, Batterispänning i mV), LWT och Gateway Web Dashboard. |

---

## 🏗️ Systemarkitektur

```mermaid
flowchart TD
    subgraph SENSOR["Verisure Sensor (CC1110)"]
        Sensors["Sensorer:\n- Reed (P1_7)\n- Shock (P1_2)\n- Tamper (P0_1)\n- Batt ADC"] --> AES["Hårdvaru AES-128\n(Device Key via KDF)"]
        AES --> DMA["DMA Trigger 19\n(RFD Buffring)"]
        DMA --> RadioTX["CC1110 RF\n868.950 MHz (27 MHz X1)"]
    end

    subgraph GW["ESP32-S2 Gateway"]
        RadioRX["CC1101 Modul\n(868.950 MHz, SPI ADC1)"] --> S2MCU["LOLIN S2 Mini\n(ESP32-S2)"]
        S2MCU --> Decrypt["AES-128 Avkodning\n& Replay-skydd"]
        Decrypt --> Discovery["HA Auto-Discovery\n& Telemetribyggare"]
        Discovery --> WebDash["Webbportal\n(HTTP 192.168.0.x)"]
        Discovery --> MQTTClient["MQTT Klient\n(PubSubClient)"]
    end

    subgraph HA["Home Assistant & Nätverk"]
        Broker["Mosquitto Broker\n(Port 1883)"] --> HA_Core["Home Assistant Core"]
        HA_Core --> Entities["Entiteter:\n- Contact (door)\n- Vibration (vibration)\n- Tamper (tamper)\n- Batteri (%)\n- Batterispänning (mV)\n- Signalstyrka (dBm)"]
    end

    RadioTX -.->|RF Paket 24B + CRC| RadioRX
    MQTTClient -->|MQTTS / TCP| Broker
```

---

## ⚡ Snabbstart

1. **Gateway IP:** [http://192.168.0.142](http://192.168.0.142) (Webbgränssnitt för Wi-Fi, MQTT och sensortelemetri).
2. **Kompilera & Flasha CC1110:**
   ```powershell
   python build_and_flash.py
   ```
3. **Bygg & Flasha Gateway:**
   ```powershell
   & "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -d 868Receiver -t upload --upload-port COM9
   ```
