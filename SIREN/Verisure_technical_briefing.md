# Technical Briefing: Reverse-Engineering & Implementation of the Verisure Siren

This document summarises all confirmed hardware architecture, RF radio configuration, cryptographic protocol structure and working methodology established during the development of Verisure's door/shock sensor (`GW-MC-SHOCK`). Use this information as a basis for analysing and implementing support for the **Verisure Siren**.

---

## 1. Radio Configuration & Physical Layer (Sub-1GHz)

The Verisure ecosystem uses a standardised Sub-1GHz RF architecture centred around the Texas Instruments transceiver family (CC1100 / CC1101 / CC1110):

* **Centre frequency:** `868.950 MHz` (the European SRD/ISM band).
* **Modulation:** `2-FSK`.
* **Data rate:** `38.383 kBaud` (nominally ~38.4 kBaud).
* **Frequency deviation:** `21.4 kHz` (`DEVIATN = 0x35`).
* **Synchronisation word (Sync Word):** `0xD3 0x91` (16-bit / 30/32 match).
* **Preamble:** 4 bytes (`0xAA`).
* **CRC:** 2 bytes, the CC1101/CC1110 CRC-16 (polynomial `0x8005`, initial value `0xFFFF`). The door sensor's CC1110 adds it in hardware; the gateway checks it in software.
* **Reference crystal (Critical trap!):**
  - The Verisure PCB uses an external crystal at **`27.000 MHz`** (marking "27.000"), **not** the standard 26.000 MHz.
  - Formula for synthesising 868.950 MHz with a 27.000 MHz crystal:
    $$\text{FREQ} = \frac{868.950 \times 10^6 \times 2^{16}}{27.000 \times 10^6} = 2\,109\,167 = \text{0x202EEF}$$
    `FREQ2 = 0x20`, `FREQ1 = 0x2E`, `FREQ0 = 0xEF`.
  - Modem configuration for 27 MHz: `MDMCFG4 = 0x8A`, `MDMCFG3 = 0x75`.
* **Transceiver Power (PA Table):** `+10 dBm` (`PA_TABLE0 = 0xC2`).
* **Radio register addresses:** on the CC1110 the radio registers sit in XDATA at addresses that
  differ from the CC1101's SPI offsets + 0xDF00 (e.g. `FSCAL0` = 0xDF1F, `PA_TABLE0` = 0xDF2E,
  0xDF26 reserved); use the SDCC `cc1110.h` names. See
  [documentation/verisure_sensor_hardware.md](../documentation/verisure_sensor_hardware.md).

---

## 2. Cryptography & Key Management (KDF)

The protocol is based on symmetric **AES-128-ECB** with a two-stage key derivation (Key Derivation Function):

1. **Master Key (Gateway / Home network):**
   - 16 bytes (128-bit, written as `<32 hex digits from derive_key.py --generate-master>`).
   - Shared by the whole system and configured in the gateway's NVS/`secrets.h`.
2. **Device ID:**
   - Each device has a unique 8-character ASCII ID printed on a label/barcode (e.g. `2JHL2MVN`).
3. **Sensor Device Key Derivation:**
   - The device's individual AES key is one AES-128-ECB encryption with the Master Key:
     `DeviceKey = AES-128-ECB(MasterKey, ID[8 ASCII bytes] followed by 8 zero bytes)`.
   - The resulting 16-byte ciphertext forms the sensor's `DEVICE_AES_KEY` (see `868Receiver/src/crypto.cpp`, `cryptoDeriveDeviceKey()`).
   - This allows the gateway, with a single Master Key, to decrypt and authenticate any paired device by reading the device's plaintext ID in the packet header.

---

## 3. Packet Structure (The door sensor's 24-byte format)

The sensor's packet consists of 24 bytes of fixed length (+ 2 bytes RF status/CRC):

| Byte offset | Field | Description |
| :--- | :--- | :--- |
| `0 .. 7` | **Header (Plaintext)** | 8-character ASCII Device ID (e.g. `"2JHL2MVN"`). |
| `8 .. 23` | **Ciphertext (AES-128)** | 16 bytes of payload encrypted with the sensor's device key. |

### Decrypted Payload (16 Bytes):
The payload layout (sequence counter, status flags, battery, ID echo, `"VS"` signature) is described in [documentation/cryptography_and_protocol.md](../documentation/cryptography_and_protocol.md). Note that the door sensor's packet counter is now 32 bits: with flag `0x20` set, its high half is in bytes 14..15.

---

## 4. Analysis of the Verisure Siren: What to investigate

Unlike the door sensor, which is primarily a **transmitter (TX-only with sleep)**, a siren is either a **receiver (RX)** or a **bidirectional node (transceiver)**:

### A. Hardware inventory (PCB & Silicon):
1. **Microcontroller / SoC:**
   - Is it based on the **TI CC1110F32** (SoC with integrated radio), or a separate MCU (e.g. PIC, STM8, MSP430) connected to a discrete **TI CC1101** via SPI?
2. **Crystal:**
   - Verify the crystal's frequency (`X1`). Is it 27.000 MHz or the standard 26.000 MHz?
3. **Power supply & Battery:**
   - How is the siren powered? (Several D/C cells, 9V, or external 12V?).
   - Low-power architecture: Does the siren listen to the radio continuously (requires ~15 mA current), or does it use **Wake-On-Radio (WOR)**, where it sleeps and only sniffs the channel periodically (e.g. every 500 ms)?
4. **Programming/Debug header:**
   - Look for test pads (like `J2` on the door sensor):
     - `VCC`, `GND`, `DC` (Debug Clock), `DD` (Debug Data), `RESET_N`.
   - Note that factory-locked CC1110 chips require a hardware **Chip Erase** to unlock the debug interface before the memory can be read or written.
5. **Siren driver stage / Piezo / Flash:**
   - Identify which GPIO pins control the MOSFET/transistor to the piezo horn and any flash LEDs.
   - Sabotage switch (Tamper switch): Identify the microswitch for the wall mount and the lid.

### B. Protocol questions for the siren:
1. **Command structure (Downlink Gateway $\rightarrow$ Siren):**
   - The siren's command must contain instructions for: *Activate siren*, *Deactivate siren*, *Strobe/flash*, *Battery status request* and possibly *Volume/Tone*.
   - What does the packet header look like? Does the gateway address the packet to the siren's specific `Device ID`?
   - Which padding and initialisation vector are used for the downlink AES block?
2. **Acknowledgement (Uplink Siren $\rightarrow$ Gateway):**
   - Does the siren send an `ACK` telemetry packet back to the gateway when triggered?
   - Does the siren send a periodic heartbeat (e.g. once per hour with battery mV and tamper status)?

---

## 5. Existing Toolchain & Flasher Infrastructure

The project has a complete, working toolchain that can be reused as is:
1. **ESP_CC_Flasher (Aaron Christophel / mDNS `http://cc.local`):**
   - ESP32-based flasher that connects to the sensor's debug header:
     - `3V3` $\rightarrow$ Target VCC
     - `GND` $\rightarrow$ Target GND
     - `GPIO 19` $\rightarrow$ `CC_DC` (Debug Clock)
     - `GPIO 23` $\rightarrow$ `CC_DD` (Debug Data)
     - `GPIO 33` $\rightarrow$ `CC_RST` (Debug Reset)
   - Supports REST calls for `/cc_init`, `/erase_cc`, `/edit` (upload `.bin`), `/flash_cc`, as well as direct dumping of flash/XDATA via `/dump_cc` and `/read_xdata`.
2. **ESP32-S2 Gateway (Receiver):**
   - WEMOS LOLIN S2 Mini connected to a CC1101 via isolated ADC1 pins (`GPIO 3, 5, 7, 9`).
   - Integrated with Home Assistant via MQTT Auto-Discovery.
   - Can be modified to transmit (TX) siren commands on the same 868.950 MHz channel.
3. **Compiler:**
   - SDCC (Small Device C Compiler) for the MCS-51 / 8051 architecture.