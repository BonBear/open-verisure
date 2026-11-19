# Changelog

All notable changes to this project. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/). A tag `v*` builds the setup package
(`release/make_release.py`) and attaches it to a GitHub release (`.github/workflows/release.yml`).

## [Unreleased]

The first release (draft for `v1.0.0`). Everything below is new.

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
  answering the camera's polls.
- Web portal and serial console: status, Wi-Fi/MQTT/master key settings, Wi-Fi scan, camera card;
  protected by a portal password (HTTP Basic, per-client lockout), CSRF and Host checks; a setup
  access point whose password is unique per gateway.
- Frequency offset setting for CC1101 modules with an off crystal.
- Built with espressif32 8.0.0 (Arduino core 3.3, ESP-IDF 5), SmartRC-CC1101 3.0.2.

### Door sensor (`VerisureDoorSensor/`, CC1110)

- Event-driven firmware: PM2 sleep, port interrupts for reed, shock and tamper, a 0.5 s tick
  (calibrated for the 36 kHz sleep clock), hourly heartbeat, watchdog, boot battery guard.
- Persistent packet counter in flash; the build script seeds a reflash from the gateway's counter.

### Camera (`MD_CAM/`, CC1110 + Blackfin)

- New Blackfin program (`snap_link`): capture from the OV7955, JPEG encoding, colour and
  grayscale, IR flash in the dark, SDRAM full frames, block deltas.
- CC1110 alarm logic: PIR and tamper events, alarm pictures while armed, polls every 30 s armed
  and 120 s disarmed, battery under load, Blackfin powered only for pictures.
- JTAG scripts to back up and write the Blackfin boot flash (converting a factory camera).

### Tools

- `build_and_flash.py` (door sensor) and `build_and_flash_md_cam.py` (camera): config.h from the
  master key, counter seed from the gateway, flashing over Wi-Fi or USB serial, `--erase` for
  debug-locked factory chips, and a check that the gateway accepts the new firmware's counter.
- `derive_key.py`: master key generation, device keys, check values.
- `release/make_release.py`: the setup package (no secrets; `--check` builds everything from it).
- Windows and Linux.

### Quality

- CI on every push and pull request: gateway and both CC1110 builds (no SDCC warnings allowed),
  a key guard against real keys in the repository, the KDF test vector, host unit and fuzz tests
  of the gateway's crypto, replay filter, portal, downlink and picture receiver, emulated-flash
  tests of the CC1110 counter log, and CodeQL. Dependabot for actions and Python packages.

### Known issues

- See `TODO.md`: e.g. rare flash faults in the counter log (being fixed), the `--gateway-restart`
  end-to-end check, the master key that was public in the history (change it before use).
