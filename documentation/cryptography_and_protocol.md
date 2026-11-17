# Protocol, RF & Cryptography: Verisure Sub-1GHz AES-128

This document describes the radio transmission specification at 868.950 MHz, the cryptographic Key Derivation Function (KDF), the packet frame layout, and the replay attack prevention implemented in the gateway.

---

## 🔑 Key Derivation Function (KDF)

The Verisure ecosystem employs a hierarchical key architecture where each physical sensor possesses a unique **Device AES Key**, derived from a central **Master Key** combined with the sensor's 8-character physical Device ID (e.g. `2JHL2MVN`).

### Mathematical Specification
$$\text{DeviceKey} = \text{AES-128-ECB}_{\text{MasterKey}}(\text{DeviceID}_{\text{ASCII}} \,\|\, \text{Padding})$$

1. **Master Key (16 bytes):** Stored on the gateway (`secrets.h` or its NVS); generate one with `python derive_key.py --generate-master`. Never use an example or sequential key.
2. **Input Block (16 bytes):** The 8 ASCII characters of the sensor ID right-padded with 8 zero bytes:
   ```text
   ['2', 'J', 'H', 'L', '2', 'M', 'V', 'N', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]
   ```
3. **Encryption:** The block is encrypted once with AES-128 in ECB mode using the Master Key. The resulting 16-byte ciphertext becomes the permanent AES-128 Device Key.
4. **Tooling & Setup Order:**
   - **Step A:** Generate your Master Key: `python derive_key.py --generate-master` and paste it into `868Receiver/src/secrets.h` as `MASTER_KEY_HEX`.
   - **Step B:** Derive individual sensor keys: `python derive_key.py --id "<ID>" --write-config`. The script automatically reads `MASTER_KEY_HEX` from `secrets.h` and writes `VerisureDoorSensor/config.h`.

---

## 📦 Packet Structure (24 Bytes + 2 Bytes Hardware CRC)

The radio transmits a fixed-length frame of **24 bytes**, followed by 2 bytes of hardware-generated CC1101/CC1110 CRC16.

```
+--------------------------------+-------------------------------------------------+
|   Bytes 0..7: Plaintext Header  |          Bytes 8..23: AES-128 Ciphertext         |
|   (8-character Device ID)      |          (Encrypted with Device Key)            |
+--------------------------------+-------------------------------------------------+
```

### 1. Plaintext Header (Bytes 0..7)
Contains the sensor's 8-character alphanumeric identifier (e.g. `2JHL2MVN`). The gateway reads this in plaintext to look up the corresponding derived device key from its active session table.

### 2. Encrypted Payload (16-Byte AES-128 Block)
Decrypted by the gateway into the following 16-byte telemetry fields:

| Byte Index | Field | Data Type | Description |
| :---: | :--- | :---: | :--- |
| **`0..1`** | **Sequence Number** | `uint16_t` (Big-Endian) | Low 16 bits of the packet counter, increments with each transmission. |
| **`2`** | **Sensor Flags** | `uint8_t` (Bitmask) | Real-time sensor state flags (see table below). |
| **`3`** | **Battery Percentage**| `uint8_t` | Approximate capacity `0`–`100%`. |
| **`4..5`** | **Battery Voltage** | `uint16_t` (Big-Endian) | Measured supply voltage in millivolts (e.g. `3370 mV` for 3.37 V). |
| **`6..9`** | **Device ID Echo** | `char[4]` | First 4 characters of Device ID (integrity check against key mismatch). |
| **`10..11`**| **Magic Bytes** | `char[2]` | Fixed signature: `'V'`, `'S'` (Verisure Sensor). |
| **`12`** | **Port 0 Diagnostics**| `uint8_t` | Raw state of CC1110 `P0` (P0_1 Tamper, P0_2/P0_4 LEDs). |
| **`13`** | **Port 1 Diagnostics**| `uint8_t` | Raw state of CC1110 `P1` (P1_7 Reed switch, P1_2 Shock). |
| **`14..15`** | **Counter, high half** | `uint16_t` (Little-Endian) | With flag bit 5: bits 16..31 of the persistent packet counter. Without it (older door firmware): `P2` and the reset cause. |

The camera uses bytes 12..15 differently (poll marker 0xBA, challenge, camera state) and also sends 56-byte picture packets and receives downlink answers; see [MD_CAM/docs/Camera_protocol.md](../MD_CAM/docs/Camera_protocol.md).

### Flag Bits (Byte 2)
```text
Bit 0 (0x01): Magnet Reed Switch (1 = Open / Alarm, 0 = Closed / Secure)
Bit 1 (0x02): Shock / Vibration Sensor (1 = Shock detected, 0 = Clear)
Bit 2 (0x04): Tamper Switch (1 = Cover removed / Alarm, 0 = Cover secured)
Bit 3 (0x08): Power-On Reset (1 = first packet after a power-on reset)
Bit 4 (0x10): Low Battery Warning (1 = Voltage below 2400 mV)
Bit 5 (0x20): Persistent counter in bytes 14..15 (door sensor firmware since 2026-11-17)
Bit 6..7:     With bit 5: reset cause SLEEP.RST[1:0] (0 = power-on, 1 = RESET_N, 2 = watchdog, 3 = clock loss)
```

---

## 🛡️ Security & Replay Attack Prevention

The gateway applies a multi-stage authentication filter:
1. **CRC-16 in software:** The gateway receives fixed 58-byte frames with the CC1101's hardware CRC off (so 24- and 56-byte packets share one setting) and checks the CC1101 CRC-16 (polynomial 0x8005, initial value 0xFFFF) after 24 or 56 bytes itself.
2. **Cryptographic Integrity Check:** The payload is decrypted using the derived device key. If the magic bytes are not `'V'`, `'S'`, or if the Device ID Echo does not match the header, the frame is rejected as spoofed or corrupted.
3. **Replay Protection with a persistent counter (flag bit 5, door sensor):**
   * The packet counter is 32 bits: bytes 0..1 and 14..15. Its upper 24 bits (a block of 256
     packets) are logged in the sensor's last two flash pages before they are used, at every boot
     and every 256 packets, so the counter never goes back, not even after a reset.
   * The gateway accepts a packet only if its counter is greater than the last accepted one.
     There is no exception for resets or tamper.
   * The log entries carry a check byte, are read back after writing and are skipped when a
     power loss cut a write or an erase short; the two pages are used in turn, so an erase never
     loses the newest entry.
   * The gateway keeps the last accepted counter per device in NVS (namespace `replay`), written
     for every accepted packet but at most every 2 s, so the floor survives a gateway restart.
     Only packets accepted less than 2 s before a gateway restart can be replayed after it.
   * Once a device has sent the new format, packets from it in the old format are dropped.
   * Reflashing a sensor erases its log. `VerisureDoorSensor/build_and_flash.py` therefore asks the
     gateway for the sensor's last accepted counter (`GET /api/replay?id=<ID>`) and builds the
     firmware with `CTR_SEED` = the block above it, so the counter simply continues.
   * Without the gateway (or with an older build) the counter starts again at 256 and the gateway
     logs `[Replay] Dropped ... 'replay reset <ID>'`. That command on the gateway's serial console
     clears the device's floor; the gateway then accepts only a boot packet (counter low byte 0) in
     the new format from it, so a recorded packet from before the reflash cannot take its place.
   * Below 2.3 V at boot the sensor neither logs nor sends; it sleeps and measures again every
     minute, so a dying battery that resets on the TX current dip cannot wear out the log.
4. **Old format (camera, older door firmware):**
   * 16-bit `seq` that restarts at 0 after a reset; the gateway requires `seq > last_seq` within a
     16-bit window. A packet with the power-on flag is accepted anyway, so a recorded power-on
     packet can be replayed; the camera still needs the persistent counter.
