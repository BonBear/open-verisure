# GW-SD3 firmware (EFR32FG14P231F256GM48)

Bare-metal OpenVerisure firmware for the Verisure GW-SD3 smoke detector, built with plain
`arm-none-eabi-gcc` + GNU make against the Silicon Labs Gecko SDK (device files, emlib and the
precompiled RAIL radio library; no Simplicity Studio).

**Current state: first RF version.** At boot and then every 10 s the detector sends an
authenticated OpenVerisure uplink packet on 868.950 MHz in the door-sensor format (8-byte
plaintext ID + one AES-128-ECB block with the persistent 32-bit counter, magic `VS` and ID echo;
see [documentation/cryptography_and_protocol.md](../../documentation/cryptography_and_protocol.md)).
The gateway decrypts it with the KDF device key and publishes it on MQTT
(`verisure/<ID>/state`) with Home Assistant discovery. Every packet has packet type `0x5D`
(smoke status, `plain[12]`) and the heartbeat bit in `plain[13]`; a gateway with smoke support
(`868Receiver/src/smoke_packet.h`) publishes it as a smoke detector, an older gateway shows it as a
door sensor (alarm = contact OPEN). Types `0x5E`/`0x5F` (temperature, humidity) are reserved and
not sent yet.
Each packet carries the battery voltage (ADC0 on AVDD, 5 V reference, read at boot and after each TX
for the next packet, since a read right after EM2 wake is up to ~120 mV low;
low-battery flag below 2400 mV). Between packets the MCU sleeps in EM2: the RTCC on the
32.768 kHz LFXO (LFRCO fallback) wakes it and RAIL is put to sleep around it. The first 5 s after
reset stay in EM0 so SWD can attach (flashing also worked while asleep on the bench).
Not done yet: smoke chamber, test button, speaker, watchdog, event-driven packets. Measured on the bench board at 3.0 V (2026-11-27): EM0 debug window ~6.4 mA, EM2
between packets ~8 µA, TX peak ~55–58 mA (stock firmware: ~2–3 µA floor, 5–16 µA average). Earlier the
same firmware drew ~23 mA in EM0 and ~1.8 mA in EM2 with DECOUPLE at 2.67 V; that state persisted
across short Otii power cycles and cleared after the board was left unpowered (cause not proven,
latch-up suspected). The board has the DC-DC inductor (VREGSW–DVDD 0.3 Ω), but
`EMU_DCDCINIT_DEFAULT` raised EM2 to ~11 mA, so the firmware leaves the reset power configuration.
`pins_init()` holds PC6 low: PC6 high switches the battery load-test MOSFET on (~220 mA through R1/R56). The chamber itself is sequenced by `IC301`/`IC81`, not by the EFR32 (see `../docs/re_worksheet.md` section 7a).

| File | Content |
| :--- | :--- |
| `src/main.c` | Clocks (38.4 MHz HFXO), reset cause, packet loop, SWD diagnostics |
| `src/packet.c` | Plaintext block, frame and CC1101 CRC-16 (poly `0x8005`, init `0xFFFF`) |
| `src/ctr_log.c` | Persistent counter: upper 24 bits logged in the last two flash pages |
| `src/radio.c` | RAIL init, fixed-length TX, RAIL sleep/wake (`RADIO_TEST_CW` adds a carrier test) |
| `src/chamber_scan.c` | Bench probe (`-DCHAMBER_SCAN` only): pulses PC6 and samples 24 GPIOs with the ADC, results in `g_scan` over SWD |
| `src/battery.c` | AVDD measurement and battery percentage |
| `src/power.c` | LFXO/RTCC setup and EM2 sleep |
| `radio/openverisure_868.radioconf` | PHY input for the Silicon Labs radio configurator |
| `src/autogen/rail_config.{c,h}` | Generated PHY (committed, so builds need no configurator) |
| `src/config.h` | Device ID + key from `derive_key.py` (**git-ignored**) |

## PHY

2-FSK, ±21.4 kHz, 38.4 kbps, MSB first, 32-bit `0xAA` preamble, 32-bit sync `D391D391` (the
CC11xx `SYNC_MODE` 30/32 sends its 16-bit word twice), no whitening, fixed 26 bytes
(24 payload + CRC-16 appended in software). Channel 0 = 868.950 MHz; channels 1–33 are a bench
sweep 868.750 + (n − 1) × 12.5 kHz used only by the carrier test.

To regenerate `src/autogen/` after changing the `.radioconf`, run the configurator from the
Gecko SDK (`platform/radio/efr32_multiphy_configurator`, part family `nixi` = xG14) with
Python 3.10 (it still needs `imp`/`distutils`) plus `numpy<1.24`, `scipy<1.11`, `lxml`, `jinja2`,
`pyyaml`:

```bash
python Efr32RadioConfiguratorUcAdapter.py <repo>/SMOKE_DETECTOR/firmware/radio/openverisure_868.radioconf \
    -o <repo>/SMOKE_DETECTOR/firmware/src/autogen
```

## Prerequisites

- Arm GNU Toolchain (`arm-none-eabi-gcc`, tested with 14.2.rel1) and GNU make
  (Windows: e.g. GnuWin32 make 3.81 from Git Bash; Linux: `make`).
- Gecko SDK `v4.4.6` (sparse checkout):

  ```bash
  git clone --depth 1 --branch v4.4.6 --filter=blob:none --sparse \
      https://github.com/SiliconLabs/gecko_sdk.git gecko_sdk_4.4.6
  cd gecko_sdk_4.4.6
  GIT_LFS_SKIP_SMUDGE=1 git sparse-checkout set --no-cone /platform/Device/SiliconLabs/EFR32FG14P/ \
      /platform/CMSIS/Core/Include/ /platform/common/inc/ /platform/emlib/ \
      /platform/radio/rail_lib/common/ /platform/radio/rail_lib/chip/efr32/efr32xg1x/
  ```

  In Git Bash prefix the last command with `MSYS_NO_PATHCONV=1`, otherwise the leading `/` is
  rewritten to the Git install path.
- `librail_efr32xg14_gcc_release.a` into
  `platform/radio/rail_lib/autogen/librail_release/`. It is a Git LFS file whose object is not
  on GitHub's LFS server; take it from the release asset `gecko-sdk.zip` (v4.4.6) and check
  SHA-256 `817188456d217bcfbe939720899bb9de2c00fbf80eb349026cc1b87e1886057a`. The library uses the
  soft-float call ABI, so the firmware builds with `-mfloat-abi=softfp`.
- `src/config.h`: `python derive_key.py --id <ID> -w -o SMOKE_DETECTOR/firmware/src/config.h`
  (build outputs contain the key as raw bytes: keep them under the git-ignored `build/`). `src/config.h.example`
  shows the format.
- pyOCD with the Silicon Labs DFP pack (once): `pyocd pack install EFR32FG14P231F256GM48`.
- A CMSIS-DAP probe (Raspberry Pi Debug Probe) on the back pads `CLK`/`DIO`/`GND`; leave `RES`
  and `SW` unconnected. Supply 3.0 V on `BAT+` with a current limit of at least 0.5 A (the board
  peaks around 330 mA). The wire antenna must be screwed down at both ends (its ground side too):
  with it loose the gateway hears nothing.

## Build & flash

```bash
make GECKO_SDK=C:/Tools/gecko_sdk_4.4.6          # build/smoke.{elf,hex,bin}
make GECKO_SDK=C:/Tools/gecko_sdk_4.4.6 flash    # tools/flash.py via pyOCD (Linux: PYTHON=python3)
make BUILD=build/cw EXTRA_CFLAGS=-DRADIO_TEST_CW  # bench carrier test (never deploy)
make BUILD=build/chamber EXTRA_CFLAGS=-DCHAMBER_SCAN  # bench GPIO/ADC chamber probe, results in g_scan (never deploy)
```

CI (job "Smoke detector" in `.github/workflows/build.yml`) runs the default and `RADIO_TEST_CW` builds on Ubuntu (the `CHAMBER_SCAN` probe is not built or analysed in CI) with the
packaged `gcc-arm-none-eabi` and `src/config.h.example`; it takes the RAIL library out of
`gecko-sdk.zip` with `.github/scripts/fetch_zip_member.py` (HTTP range requests, no 1.3 GB download).

On Windows pass `GECKO_SDK` as a `C:/...` path (the Windows toolchain does not understand
`/c/...`). The build is warning-free (`-Wall -Wextra -Werror` for project sources).

The chip must be unlocked first. A factory GW-SD3 is debug-locked (AP0 IDR `0x26E60011`); the
Series 1 `AAP DEVICE_ERASE` unlocks it but **permanently erases the stock application** (the
external `IC6` flash holds only sound data). See [../README.md](../README.md).

The counter pages (last 4 kB of flash) are outside the image, so reflashing with
`tools/flash.py` (sector erase) keeps the counter. After a full chip erase the counter restarts:
run `replay reset <ID>` on the gateway, then power-cycle the detector.

## Verify

Watch the gateway serial log: every 10 s `[AUTHENTICATED] Device: "<ID>"` with an increasing
`Seq` and `[MQTT] >> Published to verisure/<ID>/state`. Over SWD, `g_status` = 1,
`g_tx_ok` counts sent packets and `g_tx_fail` failures (attach with pyOCD
`connect_mode=attach`; the default connect resets the target).

Bench result 2026-11-26 on Unit 1 (`3EB7KFZQ`, then still built with ID `3EA8VKXK`): packets every 10.0 s, counter #2304–#2308,
all authenticated and published to MQTT, RSSI −45 dBm, frequency error −30 kHz at the gateway
(HFXO `CTUNE` not trimmed yet).

HFXO trim (2026-11-27, Unit 1): `HFXO_CTUNE` in `src/main.c` sets
`CMU_HFXOSTEADYSTATECTRL.CTUNE`. The reset default `0x168` gave −28.6 kHz at the gateway,
`0x0F0` +4.8 kHz, `0x100` 0.0 kHz and `0x110` −4.8 kHz (about 0.3 kHz per step), so the
firmware uses `0x100`. Crystals differ per board: on a new unit check `Freq. error` in the
gateway log and rebuild with `EXTRA_CFLAGS=-DHFXO_CTUNE=0x...u` if it is more than a few kHz off.
