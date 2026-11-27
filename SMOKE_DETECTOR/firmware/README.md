# GW-SD3 firmware (EFR32FG14P231F256GM48)

Bare-metal OpenVerisure firmware for the Verisure GW-SD3 smoke detector, built with plain
`arm-none-eabi-gcc` + GNU make against the Silicon Labs Gecko SDK (device files, emlib and the
precompiled RAIL radio library; no Simplicity Studio).

**Current state: first RF version.** At boot and then every 10 s the detector sends an
authenticated OpenVerisure uplink packet on 868.950 MHz in the door-sensor format (8-byte
plaintext ID + one AES-128-ECB block with the persistent 32-bit counter, magic `VS` and ID echo;
see [documentation/cryptography_and_protocol.md](../../documentation/cryptography_and_protocol.md)).
The gateway decrypts it with the KDF device key and publishes it on MQTT
(`verisure/<ID>/state`) with Home Assistant discovery; no gateway change is needed.
Each packet carries the battery voltage (ADC0 on AVDD, 5 V reference, measured before TX;
low-battery flag below 2400 mV). Between packets the MCU sleeps in EM2: the RTCC on the
32.768 kHz LFXO (LFRCO fallback) wakes it and RAIL is put to sleep around it. The first 5 s after
reset stay in EM0 so SWD can attach (flashing also worked while asleep on the bench).
Not done yet: smoke chamber, test button, speaker, watchdog, a smoke-specific packet type and HA
entities. Measured on the bench board at 3.0 V (2026-11-27): EM0 debug window ~23 mA, EM2
between packets ~1.9 mA, TX peak ~55 mA. A test without RAIL gives EM0 13.2 mA, EM2 1.83 mA and
EM4S 0.27 mA, so EM2 is far above the datasheet (~2 µA) and the cause is still open. (A constant
~222 mA load seen earlier was a solder fault at the smoke chamber pins and is gone.)

| File | Content |
| :--- | :--- |
| `src/main.c` | Clocks (38.4 MHz HFXO), reset cause, packet loop, SWD diagnostics |
| `src/packet.c` | Plaintext block, frame and CC1101 CRC-16 (poly `0x8005`, init `0xFFFF`) |
| `src/ctr_log.c` | Persistent counter: upper 24 bits logged in the last two flash pages |
| `src/radio.c` | RAIL init, fixed-length TX, RAIL sleep/wake (`RADIO_TEST_CW` adds a carrier test) |
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
```

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
