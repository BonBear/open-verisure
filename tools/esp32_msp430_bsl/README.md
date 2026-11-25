# ESP32-C3 MSP430 ROM BSL Flasher (TI SLAU319)

USB-to-BSL bridge firmware for an **ESP32-C3 Super Mini** and host CLI tool (`bsl_flash.py`) targeting the **TI MSP430F149 Rev S** (ROM BSL v1.60, devices without a dedicated `TEST` pin) on the Verisure Siren (`M1765-1`).

## Hardware Wiring (SIREN M1765-1)

Power the SIREN board from its own supply (e.g. bench supply on `CN2`, current-limited) — **do not** connect ESP32-C3 `3V3` to the target. Put **330 Ω** series resistors on all four signal lines to protect both MCUs and allow the target to stay powered independently.

| ESP32-C3 Super Mini | Series Resistor | SIREN Point | MSP430F149 (`IC1`) Function | Notes |
| :--- | :--- | :--- | :--- | :--- |
| `GPIO 0` (`PIN_TCK`) | 330 Ω | `TP64` | `TCK` (pin 57) | BSL entry clock |
| `GPIO 4` (`PIN_RST`) | 330 Ω | `TP5` | `RST/NMI` (pin 58) | Active-low reset |
| `GPIO 2` (`PIN_BSL_RX`) | 330 Ω | `TP28` | `P1.1` / BSL TXD (pin 13) | Target -> Bridge (9600 8E1) |
| `GPIO 3` (`PIN_BSL_TX`) | 330 Ω | `IC1` pin 22 | `P2.2` / BSL RXD (pin 22) | Bridge -> Target (9600 8E1) |
| `GND` | Direct wire | `TP68` | `DVSS` / Ground | Common ground reference |

> [!NOTE]
> **GPIO 2 strapping pin:** `GPIO 2` is an ESP32-C3 boot strapping pin (must not be held low during ESP32-C3 reset/boot). If your setup pulls `TP28` low at boot, change `-D PIN_BSL_RX=1` in [platformio.ini](platformio.ini) and wire `GPIO 1` -> 330 Ω -> `TP28` instead.

All bridge GPIOs default to high-impedance (`INPUT`, no internal pull-ups) on boot and after `RELEASE`. `ENTER` refuses to drive the target (`ERR RST_LOW`) if `PIN_RST` reads low.

## 1. Build and Flash the ESP32-C3 Bridge

```bash
pio run -d tools/esp32_msp430_bsl -t upload
```

## 2. Read-Only Probe (Safe)

`probe` runs `PING -> ENTER -> SYNC -> TX BSL Version (0x1E) -> RELEASE`. It **never** calls Mass Erase (`0x18`) or RX Password (`0x10`), so it will not erase the MSP430 flash:

```bash
python tools/esp32_msp430_bsl/bsl_flash.py probe
# Or specify the serial port explicitly:
python tools/esp32_msp430_bsl/bsl_flash.py probe --port COM5
```

Expected output for an MSP430F149 Rev S: `Chip ID: 0xF149 | BSL Version: 0x0160 (v1.60)`.

## 3. Erase, Program, and Verify (`flash`)

> [!CAUTION]
> Flashing via ROM BSL executes **Mass Erase (`0x18`)**, which **permanently erases all factory firmware and data** on the MSP430F149. Only run this on a board you intend to overwrite with custom firmware.

Supports Intel HEX (`.hex`, `.ihex`, `.ihx`) and TI-TXT (`.txt`) images. By default, every programmed chunk and the interrupt vector table (`0xFFE0..0xFFFF`) are read back and checksum-verified via `TX Data Block (0x14)`:

```bash
python tools/esp32_msp430_bsl/bsl_flash.py flash firmware.hex --confirm-erase
# Optional flags:
#   --port COM5       Select serial port explicitly
#   --no-verify       Skip TX Data Block read-back verification
#   --allow-info      Allow writing Info Flash (0x1000..0x10FF)
```

