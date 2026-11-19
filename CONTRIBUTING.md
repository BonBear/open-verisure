# Contributing to the Verisure Sub-1GHz Project

Thank you for your interest in contributing! This project is an open-source, community-driven effort to reverse-engineer, liberate, and give a second life to Sub-1GHz proprietary alarm hardware.

---

## 🎯 How You Can Help

### 1. Adding Support for New Verisure Hardware
Verisure manufactures a wide variety of Sub-1GHz battery-powered sensors using TI CC1110 / CC1111 / CC2510 SoCs. We are actively looking to add support for:
* **Smoke & Heat Detectors**
* **PIR Motion Sensors** (standard and photodetectors)
* **Outdoor Motion Sensors**
* **Water Leak / Flood Detectors**
* **Alarm Keypads & Keyfobs**
* **Internal / External Sirens**

If you have unused sensors, you can contribute by:
* Mapping test pads and debug headers (J2 / ISP)
* Identifying pin connections for sensors, LEDs, buttons, and crystals
* Dumping factory firmware for cryptographic and protocol analysis
* Developing new firmware under a dedicated directory (e.g. `VerisureSmokeDetector/`, `VerisureMotionSensor/`)

### 2. Improving Gateway & Integrations
* Extending Home Assistant MQTT discovery options (e.g. device triggers, actions, sensitivity controls)
* Adding Web Portal features (live RF spectrum graphing, packet waterfall, OTA updates)
* Implementing multi-device and mesh capabilities

### 3. Documentation & Schematics
* Clean vector schematics and breadboard wiring diagrams
* Troubleshooting guides (everything in the repository is written in English)

---

## 🛠️ Development & Coding Standards

### 1. Sensor Firmware (MCS-51 / 8051)
* **Compiler:** Must compile cleanly with **SDCC (Small Device C Compiler)** 4.5 or newer:
  `sdcc -mmcs51 --model-small` for the door sensor, `--model-large` for the camera
  (see the build scripts for the full options).
* **Memory Constraints:** The CC1110F32 has 32 KB Flash and 4 KB XRAM. Always allocate buffers in `__xdata` and constant lookups in `__code`.
* **Hardware Register Safety:**
  * **Radio registers:** use the SDCC `cc1110.h` names (`FSCAL0` = 0xDF1F, `PA_TABLE0` = 0xDF2E).
    The CC1110's XDATA radio map is not the CC1101 SPI offset + 0xDF00 (0xDF26 is reserved).
* **Power Management:** Code should favor low power consumption, sleep modes, and rapid burst transmissions.

### 2. Gateway Firmware (ESP32-S2)
* **Framework:** Arduino framework under **PlatformIO**.
* **Coexistence Rule:** Never route high-speed SPI pins to the ADC2 domain (GPIO 11–20) on ESP32-S2. Always use ADC1 pins (GPIO 1–10).
* **Asynchronous Services:** The main loop must never block. Use non-blocking state machines for Wi-Fi, DNS, HTTP, and MQTT.
* **Host unit tests:** The security-critical gateway code (key derivation, packet authentication, replay filter, portal password, `/api/save` JSON readers) has unit tests that run on a Linux host (or WSL) with g++, not on the ESP32: install `libmbedtls-dev g++ make` and run `make -C 868Receiver/test_host` (`T=<name>` runs only matching tests). They use stubs for the Arduino core and NVS in `868Receiver/test_host/stubs` and only public test keys; CI runs them on every push. Add a test when you change these parts. The same run includes deterministic fuzz/property tests of the radio receive path (`fuzz_*.cpp`: packet authentication and replay window, picture chunk reassembly and resend, JPEG delta splicing) under AddressSanitizer and UBSan; `make -C 868Receiver/test_host fuzz` runs only those, `FUZZ_SCALE=<n> FUZZ_SEED=<n>` runs longer or with other seeds.

### 3. Absolute Secret Hygiene
* **Never commit secrets:** Never commit real Wi-Fi passwords, MQTT credentials, or production AES master keys to version control.
* Always update `.example` template files if introducing new configuration options.

---

## 🚀 Pull Request Workflow

1. **Fork the Repository** to your own GitHub account.
2. **Create a Feature Branch:**
   ```bash
   git checkout -b feature/support-smoke-detector
   ```
3. **Test Thoroughly:**
   * Verify that the door sensor compiles: `python VerisureDoorSensor/build_and_flash.py --compile-only`.
   * Verify that the camera firmware compiles (SDCC, `MD_CAM/build_and_flash_md_cam.py`).
   * Verify that `868Receiver` builds with PlatformIO (`pio run -d 868Receiver`).
   * Run the gateway host unit tests (`make -C 868Receiver/test_host`).
   * Verify live reception and MQTT discovery.
4. **Commit with Clear Messages:**
   ```bash
   git commit -m "feat(sensor): add pinout mapping for Verisure smoke sensor"
   ```
5. **Open a Pull Request** describing your changes, test results, and any hardware specifics.
