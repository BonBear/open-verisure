# Verisure door / window sensor (GW-MC-SHOCK)

Open-source firmware for the Verisure **GW-MC-SHOCK** door/window sensor (TI CC1110F32, 8051 core,
868.950 MHz). It reports contact, shock and tamper events with hardware AES-128 to the
[gateway](../868Receiver/README.md), which publishes them to Home Assistant.

## Features

* **Fast events (< 2 ms):** door open/close (reed switch P1_7), shock/vibration (P1_2) and
  tamper (P0_1) are read every millisecond and sent right away; a heartbeat once an hour.
* **Power:** the CC1110 currently polls its inputs in a busy loop and does not sleep, which
  limits battery life; PM2 sleep with port interrupts (as in the camera) is on the
  [TODO list](../TODO.md).
* **Encrypted packets:** 24 bytes, device ID + AES-128-ECB with the device key, magic `VS`,
  device ID echo and a rolling sequence number.
* **Home Assistant entities** (created by the gateway): door contact, vibration, tamper,
  battery %, battery voltage (mV), signal strength.

## Hardware

| Part | Description |
| :--- | :--- |
| Sensor | Verisure **GW-MC-SHOCK** with TI CC1110F32 (27 MHz crystal, SAW filter) |
| Programmer | ESP32 running [ESP_CC_Flasher](https://github.com/atc1441/ESP_CC_Flasher) (or a TI CC Debugger) |
| Wires | Jumper wires or probe pins for the 7-pad `J2` header |

### Programming header J2

The sensor has an unpopulated 7-pad header **J2** (pin 1 = square pad).

| ESP32 flasher pin | J2 pad | Signal |
| :---: | :---: | :---: |
| 3V3 | 3 | VCC (target power) |
| GPIO 23 | 4 | DD (debug data) |
| GPIO 19 | 5 | DC (debug clock) |
| GPIO 33 | 6 | RESET_N |
| GND | 7 | Ground |

The pins are set in the flasher firmware (`ESP_CC_Flasher/src/main.h`: ESP32 DevKit DC = GPIO 19, DD = GPIO 23, RESET = GPIO 33; ESP32-C3 DC = GPIO 3, DD = GPIO 4, RESET = GPIO 1). An older wiring picture showed GPIO 18/19/23 and was wrong; it was removed. Connect the flasher's GND to J2-7 in any case; its 3V3 to J2-3 only if the board is not powered otherwise.

## Build and flash

Requires SDCC, Python 3 (`pip install pyserial requests cryptography`) and the ESP_CC_Flasher on
your network (default `http://cc.local`). Generate the master key and set up the gateway first
(see the [main README](../README.md#quick-start)).

1. **Derive the device key** from the 8-character ID on the sensor's label:
   ```bash
   python derive_key.py --id "2JHL 2MVN" --write-config
   ```
   This reads `MASTER_KEY_HEX` from `868Receiver/src/secrets.h` and writes
   `VerisureDoorSensor/config.h` (git-ignored; template: `config.h.example`).
2. **Build, flash and verify:**
   ```bash
   python VerisureDoorSensor/build_and_flash.py
   ```
   The script compiles `main.c` with SDCC, uploads it to the flasher over HTTP, programs and
   restarts the CC1110, and watches the gateway's serial log until the sensor's first packet is
   authenticated.
3. **Home Assistant:** the sensor appears as `Verisure Sensor <ID>` under the MQTT integration.

## Files

| Path | Content |
| :--- | :--- |
| `main.c` | Sensor firmware: GPIO, PM2 sleep, AES, radio |
| `config.h.example` | Template for the device ID and derived key |
| `docs/` | Reverse-engineering notes and board photos: [door sensor guide](docs/verisure_door_sensor_guide.md), [hardware mapping](docs/hardware_mapping.md), [CC1110 quirks, DMA & SDCC](docs/cc1110_quirks_dma_toolchain.md), [radio registers](docs/radio_registers_reference.md), [early project overview](docs/project_overview.md) |

More: [sensor PCB mapping](../documentation/verisure_sensor_hardware.md),
[protocol & cryptography](../documentation/cryptography_and_protocol.md).
