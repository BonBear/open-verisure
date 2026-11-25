# OpenVerisure SIREN Firmware (MSP430F149 + CC1101)

Phase 2 OpenVerisure firmware for the Verisure Siren (`M1765-1`, **TI MSP430F149 Rev S** + **TI CC1101** `26.000 MHz` crystal + `32.768 kHz` `EADH` LFXT1 crystal).

## Phase 2 Scope

- Persistent 32-bit rolling counter in MSP430 Info Flash (`0x1000..0x10FF`, Info B & Info A ping-pong, `< 2700 mV` write guard).
- Software AES-128-ECB encryption/decryption (zero static RAM; stack wiped on return).
- 868.950 MHz CC1101 telemetry uplinks:
  - `0x00` boot announce (with `POR` flag on first packet after power-on reset)
  - `0xB5` tamper switch `SW1` events (50 ms debounce, exponential backoff `2 s -> 300 s`, calm reset `600 s`)
  - `0xBA` periodic polls (`120 s` disarmed, `30 s` armed, `5 s` sounding) with `120 ms` downlink RX window (`DL...VS`, `0xA0` OFF / `0xA1` ON with duration in `dl_plain[8]`, tone ID `0..5` in `dl_plain[9]`, and per-siren `fast_wake` flag `0x80 | bit0` in `dl_plain[10]`; confirmed in `0xBA` state bit 3 `0x08`)
- `LPM3` deep sleep (`Timer_A` 1.000 s tick + `Port 1` tamper interrupt + `Port 2` `P2.7` CC1101 WOR interrupt; `LPM0` fallback if `LFXT1` faults).
- **Optional CC1101 Wake-on-Radio ("Fast Wake / Doorbell") mode:** Controlled per-siren via the Home Assistant `Fast Wake (Doorbell)` switch (`default OFF`).
  - When **OFF**: `CC_SPWD` sleep (`7.6 uA` LPM3 floor) with standard `120 s` / `30 s` polls.
  - When **ON**: CC1101 runs 1.0 s `SWOR` (`~21.4 uA` standalone WOR idle) on **869.525 MHz** (Band 54, 10% duty cycle) with dedicated sync `0x2DD4` and 24-bit FNV-1a per-siren addressing (`ADDR` + `tag16`) so `MD_CAM` picture chunks (`0xD391`) and other sirens are ignored. A wake packet never plays audio directly — it only triggers a `TAR`-aligned authenticated `0xBA` poll on `868.950 MHz` (~1–2 s end-to-end latency), protected by anti-DoS cooldown/backoff (`5 -> 10 -> 20 -> 40 s`) and 5-strike `300 s` `SPWD` pause.
- **OKI L22901-707 (`IC2`) parallel audio driver:** `P6.0..P6.5` (`P6DIR = 0x3F`, `P6.4` falling-edge strobe) with active-LOW audio/op-amp rail `P1.4` (`1` = OFF idle, `0` = ON while sounding). Supports tone IDs `0..5` (`alarm`, `ding_dong`, `chime`, `double_beep`, `beep`, `tone`), auto-retriggering every `2 s` for `alarm` or durations `> 2 s`.
- **Safety:** High-voltage horn boost converter (`P1.7` / `IC9`, active-HIGH) stays **INPUT (Hi-Z)** by default (`7.6 uA` LPM3 idle; driving `P1.7` `OUTPUT LOW` sinks `12.05 mA`) and is only driven `OUTPUT HIGH` while sounding if `SIREN_ENABLE_HV_BOOST` is explicitly defined at compile time.

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

| Pin | Signal | Direction / Mode | Phase 2 State |
| :--- | :--- | :--- | :--- |
| `P1.4` | `TP2` / `Q3` audio power rail (active-LOW) | Output | `1` (OFF) idle, `0` (ON) while sounding |
| `P1.6` | `SW1` tamper switch | Input (no `PxREN`) | `P1IE` dynamic edge IRQ |
| `P1.7` | `IC9` horn boost enable (active-HIGH) | Input (Hi-Z) idle / Output if enabled | `INPUT` Hi-Z (`7.6 uA` LPM3); `OUTPUT HIGH` while sounding only with `SIREN_ENABLE_HV_BOOST` |
| `P2.6` | CC1101 `GDO0` | Input | Sync / packet status |
| `P2.7` | CC1101 `GDO2` | Input | WOR packet IRQ (`PORT2_VECTOR` falling edge when Fast Wake ON; `HW0` low when OFF) |
| `P4.7` | `LED1` (active-high via `Q1`) | Output | `0` idle, brief blink on TX/boot/sounding |
| `P5.0` | CC1101 `CSn` | Output | `1` idle |
| `P5.1` | CC1101 `SIMO1` | Peripheral Out (`USART1`) | SPI Mode 0 |
| `P5.2` | CC1101 `SOMI1` | Peripheral In (`USART1`) | SPI Mode 0 |
| `P5.3` | CC1101 `UCLK1` | Peripheral Out (`USART1`) | SPI Mode 0 |
| `P6.0..P6.5` | OKI `L22901-707` (`IC2`) parallel bus | Digital Output (`P6DIR = 0x3F`) | `0` idle; `P6.4` falling-edge strobe, `P6.0` STOP, `(P6.5, P6.3, P6.2)` tone |
| `P6.6` | Unmapped | Input (`P6DIR` bit 6 = `0`) | Preserved untouched (`~0x3FU` mask) |
| `P6.7` | `TH1` thermistor (`A7`) | Analog (`P6SEL = BIT7`) | ADC12 analog input |
| Unmapped | `P1.0..3,5`, `P2.0..5`, `P3.0..7`, `P4.0..6`, `P5.4..7` | Output | `0` (LOW) |

