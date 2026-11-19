# Gateway (ESP32-S2 + CC1101)

PlatformIO firmware for a **WEMOS LOLIN S2 Mini** with a **CC1101 868 MHz** module. It receives
and decrypts the Verisure devices' packets, publishes them to Home Assistant over MQTT with
auto-discovery, answers the camera's polls with the alarm state and picture commands, and
reassembles camera pictures.

## Hardware

| ESP32-S2 pin | CC1101 pin | Signal |
| :---: | :---: | :--- |
| 3V3 | VCC | 3.3 V |
| GND | GND | Ground |
| GPIO 3 | CSN | SPI chip select |
| GPIO 5 | MOSI | SPI data out |
| GPIO 7 | SCK | SPI clock |
| GPIO 9 | MISO / GDO1 | SPI data in |

> [!IMPORTANT]
> Use these ADC1-domain pins. The default FSPI pins (GPIO 11/12) put SPI noise into the
> ESP32-S2's Wi-Fi receiver. Background: [gateway hardware](../documentation/hardware_gateway_esp32s2.md).

![Gateway wiring](../documentation/images/wiring_gateway_s2mini_cc1101.jpg)

## Build and flash

```bash
cp 868Receiver/src/secrets.h.example 868Receiver/src/secrets.h   # your master key (git-ignored)
pio run -d 868Receiver -t upload --upload-port <gateway port>
```

`secrets.h` only needs `MASTER_KEY_HEX`; Wi-Fi, MQTT and the web portal password are optional
there. Without Wi-Fi the gateway opens the setup access point `Verisure-Gateway` (password
`12345678`, page `http://192.168.4.1`). Settings can be changed later in the web portal (stored
in NVS).

## What it does

* **Radio:** 868.950 MHz, 38.4 kBaud 2-FSK. The CC1101 receives fixed 58-byte frames with the
  CRC checked in software: a CRC after 56 bytes is a camera picture packet (type 0x82, 40 bytes of
  JPEG), after 24 bytes a normal device packet. The FIFO is read in pieces while a frame comes in,
  and RX is not flushed between packets, so back-to-back picture packets are not lost.
* **Security:** per-device key derived from the master key, AES-128 decryption, `VS` magic and
  device ID echo, and a replay filter: the door sensor and the camera send a 32-bit counter that
  never goes back (kept in their flash), which must increase with no exception for resets or
  tamper; its last value is kept in NVS (`replay`) across gateway restarts. Details:
  [cryptography & protocol](../documentation/cryptography_and_protocol.md).
* **Home Assistant:** MQTT auto-discovery per device (door sensor entities; camera entity, motion
  and buttons "Take 640x480 picture" and "Take flash picture" for the camera), an alarm switch, and
  availability (LWT).
* **Camera downlink:** answers each camera poll within ~0.1 s with the alarm state and a queued
  picture command (one per camera); the answer echoes the poll's 32-bit counter.
* **Pictures:** chunks are placed by number; a picture ends with its last chunk, the next
  picture's first chunk or 5 s of silence. Full 320x240 pictures are the base that deltas are
  spliced into; if a delta has no base the gateway asks the camera for a full picture. Grayscale
  flash pictures and 640x480 pictures are kept apart. Every picture is published on MQTT.
* **Resend:** a full picture with missing chunks is published as it is, then the gateway asks the
  camera (with the answer to the poll that follows the picture) for the missing chunks among the
  32 from the first missing one (a bitmap), repeatedly, until it is complete (tested with every
  7th chunk dropped: a 640x480 picture complete after 14 rounds in 2.2 s); the complete picture
  is stored and published again.

## MQTT topics (base topic `verisure`)

| Topic | Direction | Payload |
| :--- | :--- | :--- |
| `verisure/<id>/state` | out | device telemetry (JSON) |
| `verisure/<id>/image` | out | latest camera picture (JPEG) |
| `verisure/<id>/picture/set` | in | `D1`..`D6` picture command for a camera; `HOLD` keeps its Blackfin on 10 min (service) |
| `verisure/alarm/set` | in | `ON` / `OFF` (or alarm_control_panel states) |
| `verisure/status` | out | `online` / `offline` (LWT) |

More: [Home Assistant & MQTT](../documentation/homeassistant_mqtt.md).

## Web portal and serial console

* **Web portal** (port 80, captive portal on first start): status, Wi-Fi/MQTT/key settings, sensor
  list. API: `/api/status`, `/api/replay?id=<ID>` (a sensor's last accepted counter), `/api/radio`, `/api/scan`, `/api/save`, `/api/reboot`, `/api/reset`,
  `/api/camera.jpg` (newest 320x240 picture, colour or flash), `/api/camera_large.jpg` (640x480),
  `/api/camera_test?id=<ID>` (POST: publishes a small test JPEG to MQTT), `/api/picture?id=<ID>&cmd=D1`
  (POST: picture request). With a camera the page shows its last picture and picture buttons.
* **Portal password and access rules:** the page and every API call except `/api/replay` need
  the web portal password (8-64 characters; the browser asks for it, HTTP Basic auth, any user
  name). It is chosen on the first save in the setup access point (required there), set with
  the serial command `portalpass <password>`, or taken from `DEFAULT_PORTAL_PASS` in `secrets.h`
  while none is stored; only a salted hash is kept in NVS. Without one the API only answers in
  the setup access point. Five wrong passwords lock the check for 30 s. `/api/save`,
  `/api/reboot`, `/api/reset`, `/api/camera_test` and `/api/picture` must be POSTs with the header
  `X-Gateway: 1` and no foreign `Origin`, and every request needs a `Host` that names the gateway
  (its IP or `verisure-gateway[.local]`). The password crosses the network as plain HTTP: see
  [SECURITY.md](../SECURITY.md).
* **Serial console** (USB, 115200): `help`, `status`, `testspi`, `scan`,
  `connect <ssid> [password]` (join Wi-Fi and save it), `setkey <32 hex characters>` (master key),
  `setmqtt <host> <port> [user] [pass] [topic]`, `alarm [on|off]`,
  `drop <n>` (test: drop every n-th picture chunk to exercise the resend),
  `replay reset <ID>` (forget a sensor's counter after it was flashed without the gateway; then
  only a boot packet of counter blocks 1..8, an unseeded flash, is taken),
  `freqoffset [n]` (CC1101 frequency correction in 1.587 kHz steps, default 0; see
  [hardware_gateway_esp32s2.md](../documentation/hardware_gateway_esp32s2.md)),
  `portalpass [<password>|clear]` (show, set or clear the web portal password), `reset` (erase
  all NVS settings), `reboot`. The master key is never printed, only its check value.

## Source files

| File | Content |
| :--- | :--- |
| `main.cpp` | CC1101 setup, RX loop with software CRC, poll answers |
| `crypto.cpp/h` | Key derivation, AES, packet parsing, replay filter |
| `downlink.cpp/h` | Alarm state and queued picture commands for the cameras |
| `image_receiver.cpp/h`, `jpeg_splice.cpp/h` | Picture reassembly and delta splicing |
| `mqtt_manager.cpp/h`, `ha_discovery.cpp/h` | MQTT client, discovery, commands |
| `web_portal.cpp/h`, `serial_cli.cpp/h` | Web portal and serial console |
| `config_manager.cpp/h`, `sensor_registry.cpp/h` | NVS settings, device cache |
| `hardware_pins.h` | CC1101 SPI pins |
