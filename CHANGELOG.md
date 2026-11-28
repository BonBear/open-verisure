# Changelog

All notable changes to this project. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/). A tag `v*` builds the setup package
(`release/make_release.py`) and attaches it to a GitHub release (`.github/workflows/release.yml`).

## [Unreleased]

### CI

- New job "Smoke detector (EFR32FG14, arm-none-eabi-gcc)": builds `SMOKE_DETECTOR/firmware`
  (normal and carrier-test build, warnings are errors) against a cached sparse Gecko SDK v4.4.6;
  the RAIL library is cut out of the 1.3 GB release zip with HTTP range requests
  (`.github/scripts/fetch_zip_member.py`) and checked by SHA-256. cppcheck also covers the smoke
  detector sources.

### Gateway

- Web portal and CLI `status` show smoke detectors with smoke wording (portal SMOKE!/Clear, CLI SMOKE ALARM/Clear; Chamber:
  OK/FAULT/DIRTY/TEST/HUSHED) instead of door contact/shock; `/api/status` sensors carry `smoke_status`.
- Smoke detector class: packets with `plain[12]` = `0x5D` (status), `0x5E` (temperature) or `0x5F`
  (humidity) are published as a smoke detector (smoke, chamber fault/dirty, test, tamper, battery,
  signal, temperature, humidity) with its own HA device block; the last values are kept between packet
  types; a device first seen as a door sensor is announced again. Host tests in `test_smoke.cpp`.

### Smoke detector

- Bench probe `-DCHAMBER_SCAN` and corrected chamber mapping: `IC301` sequences `IC81`/`LED81`, `PC6` is a battery load test, the test button is `PA0` (active low).
- Packets carry the smoke status type `0x5D` and the heartbeat bit (`PKT_TYPE_SMOKE_STATUS`,
  `PKT_SMOKE_*`); types `0x5E`/`0x5F` are reserved for temperature and humidity. An older gateway
  shows the detector as a door sensor.
- HFXO `CTUNE` trimmed to `0x100` (`HFXO_CTUNE`): carrier error at the gateway went from −28.6 kHz
  to 0.0 kHz on the bench unit.

### Tools

- `tools/si446x_spi_decode.py`: decodes a Saleae SPI capture of a Si4460/Si446x radio (voicebox)
  into the command log, property image, TX/RX payloads and derived RF settings.

### Voicebox

- Corrected MSP430F5435A PN pin references (XT1 pins 13/14, BSL `P1.1`/`P1.2`), added the verified
  PN pin table, extended the bench worksheet, and added front/back photo overlays that project the
  MSP430 pins onto the pogo cluster and the Si4460 onto the back test pads.

## [1.0.0] - 2026-11-22

The first release. Everything below is new.

### Gateway (`868Receiver/`, ESP32-S2 + CC1101)

- Receives and authenticates AES-128 packets from Verisure door/window sensors and the GW MD CAM2
  motion camera at 868.950 MHz; per-device keys derived from one master key
  (`DeviceKey = AES(MasterKey, Pad16(DeviceID))`), only check values are ever printed.
- Replay protection: a 32-bit packet counter per device that survives resets on both sides (the
  device logs it in flash, the gateway keeps the last accepted value in NVS); `replay reset <ID>
  [block]` for an unseeded reflash.
- Home Assistant MQTT discovery: door sensor (contact, vibration, tamper, battery, voltage, low
  battery, signal) and camera (motion, tamper, battery incl. under load, low battery, camera
  picture, picture buttons), `expire_after` and availability.
- Camera support: pictures in 56-byte chunks with resend requests, block deltas spliced into the
  last picture, 640x480 and flash pictures, alarm state and picture requests over an RF downlink
  answering the camera's polls, and per-camera picture slots in PSRAM (with a 16-bit MQTT camera
  image length guard).
- Dual-CC1101 869.525 MHz picture channel (Phases A–E): optional second receive-only CC1101 on
  `CS2 = GPIO 2` (`pch cal` / `freqoffset2`), `DL_FLAG_PCH` advertisement with a per-camera miss
  detector, `0xBB`/`0xB1` picture token arbitration, rolling 7x10-min duty-cycle and band-50
  fallback tracking, `duty` / `pch` CLI commands, and Radio #2 / token / duty / fallback web portal
  tiles.
- Web portal and serial console: status, Wi-Fi/MQTT/master key settings, Wi-Fi scan, camera card;
  protected by a portal password (HTTP Basic, per-client lockout), CSRF and Host checks; a setup
  access point whose password is unique per gateway.
- Reliability: dedicated CC1101 FreeRTOS radio task (only SPI owner, PSRAM frame queue),
  `GuardedWebServer` with bounded request/write times, and a 5 s loop watchdog.
- Firmware update over Wi-Fi: `POST /api/ota` (portal password, optional MD5) or
  `868Receiver/ota_upload.py`; refused in setup-AP mode, and a new image that does not reach
  Wi-Fi within 5 min rolls back to the previous one.
- Diagnostics in Home Assistant: uptime, reset reason, free heap, and counters of received,
  authenticated, rejected and replayed packets, stale picture chunks and the last rejected ID.
- Frequency offset setting for CC1101 modules with an off crystal (`freqoffset` / `freqoffset2`).
- Built with espressif32 8.0.0 (Arduino core 3.3, ESP-IDF 5) and our own minimal static CC1101
  SPI driver (replacing `SmartRC-CC1101 3.0.2`).

### Door sensor (`VerisureDoorSensor/`, CC1110)

- Event-driven firmware: PM2 sleep, port interrupts for reed, shock and tamper, a 0.5 s tick
  (calibrated for the 36 kHz sleep clock), hourly heartbeat, watchdog, boot battery guard,
  rattling-reed exponential backoff (`2 s -> 5 min`, 10 min calm reset), and Listen-Before-Talk
  CCA (`cc1110_common/radio_cca.c`, Phase F).
- The 0.5 s tick runs on the internal RC oscillator and starts the crystal only before a packet:
  its share of the idle current fell from ~21 uA to ~4.7 uA (3.0 V, measured over 9 h; heartbeat
  drift ~+0.1 %).
- Persistent packet counter in flash; the build script seeds a reflash from the gateway's counter
  and, when flashing over USB serial, also from the device's own flash log (`0x7800-0x7FFF`, read
  with the flasher's `READ` command): the seed is `max(gateway seed, logged block + 3)`.

### Door sensor and camera (counter log)

- The flash log of the packet counter never reuses a counter value, also after a power cut during
  a flash write, a failed erase, worn cells or a missing crystal: each entry carries a 0-bit
  count check, a packet goes out only if its block is logged, and a camera picture starts only
  if its start packet is sent (emulated-flash tests in CI, bench-tested on both devices).

### Camera (`MD_CAM/`, CC1110 + Blackfin)

- New Blackfin program (`snap_link`): capture from the OV7955, JPEG encoding, colour and
  grayscale, IR flash in the dark, SDRAM full frames, block deltas.
- CC1110 alarm logic: PIR and tamper events, alarm pictures while armed, polls every 30 s armed
  and 120 s disarmed, ACK recheck poll, battery under load, Blackfin powered only for pictures.
- RF coordination and duty cycle: 869.525 MHz picture-channel switching, `0xBB`/`0xB1` picture
  token handshake, 7x10-min per-band duty-cycle self-accounting, and shared Listen-Before-Talk CCA
  (`cc1110_common/radio_cca.c`, Phase F).
- JTAG scripts to back up and write the Blackfin boot flash (converting a factory camera).

### Tools

- `build_and_flash.py` (door sensor) and `build_and_flash_md_cam.py` (camera): config.h from the
  master key, counter seed from the gateway, flashing over Wi-Fi or USB serial, `--erase` for
  debug-locked factory chips, and a check that the gateway accepts the new firmware's counter.
- `derive_key.py`: master key generation, device keys, check values.
- `release/make_release.py`: the setup package (no secrets; `--check` builds everything from it).
- Two CC flashers on one bench using the published
  [BonBear/ESP_CC_Flasher](https://github.com/BonBear/ESP_CC_Flasher) fork: an ESP32 DevKit for
  the door sensor and an ESP32-C3 Super Mini for the camera (`documentation/hardware_cc_flasher.md`),
  each with its own mDNS name.
- Windows and Linux.

### Quality

- CI on every push and pull request: gateway and both CC1110 builds (no SDCC warnings allowed),
  a key guard against real keys in the repository, `cppcheck`, `gitleaks`, the KDF test vector,
  host unit and fuzz tests of the gateway's crypto, replay filter, portal, downlink, picture
  receiver, `picture_token`, and `duty_cycle`, emulated-flash tests of the CC1110 counter log,
  pytest tests of the Python scripts, Python lint (`ruff`), a release package check, and CodeQL. `main` is protected: changes land through pull
  requests with these checks green. Dependabot for actions and Python packages.

### Known issues

- Once, after a 90 s power cut, the camera stayed awake without sending until it was reset
  (1 of 6 power cuts on 2026-11-19); since pull request #22 the watchdog starts before the start-up,
  so such a hang resets the chip.
- More in `TODO.md`, including the master key that was public in the history (change it before
  use).
