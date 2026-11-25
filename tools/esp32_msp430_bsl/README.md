# ESP32-C3 MSP430 ROM BSL Flasher (TI SLAU319)

USB-to-BSL bridge firmware for an **ESP32-C3 Super Mini** and host CLI tool (`bsl_flash.py`) targeting the **TI MSP430F149 Rev S** (ROM BSL v1.10 and v1.60, devices without a dedicated `TEST` pin) on the Verisure Siren (`M1765-1`).

## Hardware Wiring (SIREN M1765-1)

Power the SIREN board from its own current-limited bench supply on **`CN5`/`CN6`/`CN7` (`TP1A`/`TP1B`/`TP1C`) and `TP68` (`GND`)** — **NEVER** connect power to `CN2` (piezo horn H-bridge output), and **do not** connect ESP32-C3 `3V3` to the target. Put **330 Ω** series resistors on all four signal lines to protect both MCUs and allow the target to stay powered independently.

> [!TIP]
> **BSL v1.10 & reliable flashing:** The Verisure Siren (`MSP430F149 Rev S`) runs **ROM BSL v1.10** (`0x0FF0..0x0FFF = F149...0110`), which lacks `TX BSL Version (0x1E)` and ignores direct flash writes `>= 0x8000`. `bsl_flash.py` automatically detects `bsl_ver < 0x0160` and uploads an 80-byte PIC RAM flash-writer stub (`0x0300`) that copies each chunk from a RAM buffer at `0x0600` (RAM `0x0400..0x05FF` corrupted bytes on the bench, so the tool also reads the buffer back and rewrites it before running the stub: flash cannot be rewritten without an erase). For maximum reliability during flash programming, supply **3.3 V** (gives +0.6 V headroom above the MSP430F149 2.7 V flash programming minimum, safely within the 3.6 V limit of all board ICs) and keep chunk sizes `<= 96 B` to stay inside the ESP32-C3 USB-CDC 256-byte RX buffer.

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

