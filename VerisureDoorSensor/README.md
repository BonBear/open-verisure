# Verisure door / window sensor (GW-MC-SHOCK)

Open-source firmware for the Verisure **GW-MC-SHOCK** door/window sensor (TI CC1110F32, 8051 core,
868.950 MHz). It reports contact, shock and tamper events with hardware AES-128 to the
[gateway](../868Receiver/README.md), which publishes them to Home Assistant.

## Features

* **Event driven, sleeping in PM2:** the CC1110 sleeps in PM2 and wakes on port interrupts from
  the reed switch (P1_7, door open/close), the shock sensor (P1_2) and the tamper switch (P0_1),
  and on a 0.5 s sleep-timer tick. Idle current is about 17 uA. A heartbeat goes out every
  3600 s, and a ~1 s watchdog resets the chip if it ever hangs.
* **Encrypted packets:** 24 bytes, the 8-byte plaintext device ID followed by one AES-128-ECB
  block encrypted with the device key. The block carries a 32-bit persistent packet counter
  (low 16 bits in bytes 0..1, high 16 bits in bytes 14..15, flag 0x20), flags (0x01 reed open,
  0x02 shock, 0x04 tamper, 0x08 first packet after power-on, 0x10 low battery below 2400 mV,
  bits 7:6 the reset cause: 0 power-on, 1 RESET_N, 2 watchdog, 3 clock loss), the battery level
  and a `VS` magic. Full format:
  [cryptography_and_protocol.md](../documentation/cryptography_and_protocol.md).
* **Replay-safe counter:** the upper 24 counter bits are logged in flash pages 30-31
  (0x7800-0x7FFF), so the counter never goes back, not even after a reset or battery change;
  code is therefore linked with `--code-size 0x7800`.
* **Boot battery guard:** below 2.3 V the sensor neither logs nor sends; it sleeps and measures
  again about every minute.
* **Home Assistant entities** (created by the gateway): door contact, vibration, tamper,
  battery %, battery voltage (mV), signal strength.

**Status:** verified on the bench (2026-11-17): PM2 sleep (idle ~17 µA), heartbeat, boot, and the
events: reed open/close (a packet per change, ~0.33 s apart when toggled fast), shock and tamper
(pressed/released), each sent at once; the gateway logs a frequency error of +3.2 kHz. Idle current
at 3.0 V is the same with the door closed and the tamper switch pressed (lid on) as with them open:
17.9 µA (Otii, 4 minutes), so the pull-ups dropped in PM2 for closed switches work.

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

Requires SDCC, Python 3 (`pip install -r requirements.txt`) and the ESP_CC_Flasher on
your network (default `http://cc.local`). Generate the master key and set up the gateway first
(see the [main README](../README.md#setup-step-by-step)).

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
   The script compiles `main.c` with SDCC, uploads it to the flasher over HTTP (`POST /cc_init`,
   `POST /edit`, `POST /flash_cc?file=main.bin`, polls `/get_status`, `POST /cc_exit_dbg`),
   restarts the CC1110 and watches the gateway's serial log until the sensor's first packet is
   authenticated.

   Before compiling, it reads the gateway's last accepted counter for the sensor
   (`GET <gateway>/api/replay?id=<ID>`) and builds with `-DCTR_SEED`, so a reflashed sensor's
   counter continues above it (flashing erases the counter log). If the gateway cannot be
   reached the script stops: pass its address with `--gateway http://<ip>` (when `.local`
   names do not resolve on your computer), or flash with `--no-seed` (counter from block 1) and
   run `replay reset <ID>` on the gateway's serial console afterwards, or the gateway drops the
   sensor's packets as replays. A sensor the gateway has never seen needs neither.

   | Option | Effect |
   | :--- | :--- |
   | `--id <label ID>` | Write `config.h` for this sensor first (key from `secrets.h`) |
   | `--serial <port>` | Flash over the flasher's USB serial port instead of Wi-Fi (no monitor) |
   | `--compile-only` | Only build `build/main.bin` |
   | `--erase` | Full chip erase first (unlocks a protected or factory CC1110) |
   | `--gateway <url>` | Gateway HTTP address for the counter seed (env `GATEWAY_HTTP`, default `http://verisure-gateway.local`) |
   | `--no-seed` | Flash without asking the gateway (counter from block 1; needs `replay reset <ID>` if the gateway knew the sensor) |

   `python VerisureDoorSensor/build_and_flash.py --help` lists the rest (flasher host, gateway
   serial port, monitor duration).
3. **Home Assistant:** the sensor appears as `Verisure Sensor <ID>` under the MQTT integration.

## Files

| Path | Content |
| :--- | :--- |
| `main.c` | Sensor firmware: GPIO, PM2 sleep, persistent counter, AES, radio |
| `build_and_flash.py` | Build with SDCC, seed the counter from the gateway, flash and verify |
| `config.h.example` | Template for the device ID and derived key |

## Docs and photos

Board, pinout, `J2`, radio registers and the radio pitfalls:
[documentation/verisure_sensor_hardware.md](../documentation/verisure_sensor_hardware.md). Notes in
[docs/](docs/):

* [cc1110_quirks_dma_toolchain.md](docs/cc1110_quirks_dma_toolchain.md): CC1110 vs CC1101
  differences, DMA trigger 19, SDCC flags and the flasher's HTTP flow.

Photos in `docs/`:

* [pcb_bottom_mapped.png](docs/pcb_bottom_mapped.png): bottom side with `J2`, U1, battery
  connections and sensors marked.
* [fig_j2_connector_detail.jpg](docs/fig_j2_connector_detail.jpg): the `J2` flashing header (pin 1
  square pad).
* [fig_top_sensors_sw1.jpg](docs/fig_top_sensors_sw1.jpg): top side with battery clip, button
  `SW1` and reed switch.
* [fig_cc1110_rf_crystal.jpg](docs/fig_cc1110_rf_crystal.jpg): the CC1110, the 27.000 MHz crystal
  `X1`, balun and RF path.
* [door_board_top.jpg](docs/door_board_top.jpg),
  [door_board_bottom_1.jpg](docs/door_board_bottom_1.jpg),
  [door_board_bottom_2.jpg](docs/door_board_bottom_2.jpg): full board photos, top and bottom.
* [door_board_top_reed_antenna.jpg](docs/door_board_top_reed_antenna.jpg): top side with the reed
  switch and the antenna.
* [door_board_top_shock_sensor.jpg](docs/door_board_top_shock_sensor.jpg): top side with the
  shock sensor.
* [door_crystal_x1_closeup.jpg](docs/door_crystal_x1_closeup.jpg),
  [door_crystal_x1_area.jpg](docs/door_crystal_x1_area.jpg),
  [door_crystal_x1_crop.jpg](docs/door_crystal_x1_crop.jpg): the crystal `X1` and its marking
  "27.000".
* [door_saw_filter_u2_1.jpeg](docs/door_saw_filter_u2_1.jpeg),
  [door_saw_filter_u2_2.jpeg](docs/door_saw_filter_u2_2.jpeg): microscope images of the balun and
  the SAW filter `U2` (EPCOS B3716, 868-870 MHz).
* [door_cc1110_chip.jpeg](docs/door_cc1110_chip.jpeg): microscope image of the CC1110F32.

More: [sensor PCB mapping, J2 and radio registers](../documentation/verisure_sensor_hardware.md),
[protocol & cryptography](../documentation/cryptography_and_protocol.md).
