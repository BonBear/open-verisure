# OpenVerisure SIREN Firmware (MSP430F149 + CC1101)

Phase 1 OpenVerisure firmware for the Verisure Siren (`M1765-1`, **TI MSP430F149 Rev S** + **TI CC1101** `26.000 MHz` crystal + `32.768 kHz` `EADH` LFXT1 crystal).

## Phase 1 Scope

- Persistent 32-bit rolling counter in MSP430 Info Flash (`0x1000..0x10FF`, Info B & Info A ping-pong, `< 2700 mV` write guard).
- Software AES-128-ECB encryption/decryption (zero static RAM; stack wiped on return).
- 868.950 MHz CC1101 telemetry uplinks:
  - `0x00` boot announce (with `POR` flag on first packet after power-on reset)
  - `0xB5` tamper switch `SW1` events (50 ms debounce, exponential backoff `2 s -> 300 s`, calm reset `600 s`)
  - `0xBA` periodic polls (`120 s` disarmed, `30 s` armed) with `120 ms` downlink RX window (`DL...VS`)
- `LPM3` deep sleep (`Timer_A` 1.000 s tick + `Port 1` tamper interrupt; `LPM0` fallback if `LFXT1` faults).
- **Safety:** Horn boost converter (`P1.7` / `IC9`) and audio power rail (`P1.4` / `TP2`/`Q3`) are held strictly **LOW (OFF)** throughout Phase 1. Horn/OKI sounder control is deferred to Phase 2.

## Configuration

Generate your device-specific `include/config.h` (git-ignored) from your gateway master key:

```bash
python derive_key.py --id "YOUR8ID1" --out SIREN/firmware/include/config.h
```

Alternatively, copy `include/config.h.example` to `include/config.h` for local testing.
Use `-DCTR_SEED=<block>` when rebuilding after a flash erase so the counter continues above the gateway's last accepted value.

## Build

```bash
# Local build using include/config.h
pio run -d SIREN/firmware

# CI / example-config build using include/config.h.example
pio run -d SIREN/firmware -e siren_f149_ci
```

## Flashing (ESP32-C3 ROM BSL Bridge)

`bsl_flash.py` mass-erases Main Flash (and Info Flash if unprotected) before programming; always verify backups first and confirm the erase flag explicitly:

```bash
python tools/esp32_msp430_bsl/bsl_flash.py flash --port COMx SIREN/firmware/.pio/build/siren_f149/firmware.hex --confirm-erase
```

## Pin Map (MSP430F149)

| Pin | Signal | Direction / Mode | Phase 1 State |
| :--- | :--- | :--- | :--- |
| `P1.4` | `TP2` / `Q3` audio power rail | Output | `0` (OFF) |
| `P1.6` | `SW1` tamper switch | Input (no `PxREN`) | `P1IE` dynamic edge IRQ |
| `P1.7` | `IC9` horn boost enable | Output | `0` (OFF) |
| `P2.6` | CC1101 `GDO0` | Input | Sync / packet status |
| `P2.7` | CC1101 `GDO2` | Input | High-Z input |
| `P4.7` | `LED1` (active-high via `Q1`) | Output | `0` idle, brief blink on TX/boot |
| `P5.0` | CC1101 `CSn` | Output | `1` idle |
| `P5.1` | CC1101 `SIMO1` | Peripheral Out (`USART1`) | SPI Mode 0 |
| `P5.2` | CC1101 `SOMI1` | Peripheral In (`USART1`) | SPI Mode 0 |
| `P5.3` | CC1101 `UCLK1` | Peripheral Out (`USART1`) | SPI Mode 0 |
| `P6.0..P6.7` | Analog / `TH1` (`A7` = `P6.7`) | `P6SEL = 0xFF` | Analog input (0 uA digital draw) |
| Unmapped | `P1.0..3,5`, `P2.0..5`, `P3.0..7`, `P4.0..6`, `P5.4..7` | Output | `0` (LOW) |

