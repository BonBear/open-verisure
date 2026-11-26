# GW-SD3 firmware (EFR32FG14P231F256GM48)

Bare-metal OpenVerisure firmware for the Verisure GW-SD3 smoke detector, built with plain
`arm-none-eabi-gcc` + GNU make against the Silicon Labs Gecko SDK device files (no Simplicity Studio).

**Current state: bring-up test.** `src/main.c` pulses the smoke-chamber LED `LED81` (`PF4`) for
100 µs every 500 ms and increments `g_heartbeat` in RAM. No radio yet.

## Prerequisites

- Arm GNU Toolchain (`arm-none-eabi-gcc`, tested with 14.2.rel1) and GNU make
  (Windows: e.g. GnuWin32 make 3.81 from Git Bash; Linux: `make`).
- Gecko SDK `v4.4.6` device files (sparse checkout, ~30 MB):

  ```bash
  git clone --depth 1 --branch v4.4.6 --filter=blob:none --sparse \
      https://github.com/SiliconLabs/gecko_sdk.git gecko_sdk_4.4.6
  cd gecko_sdk_4.4.6
  git sparse-checkout set --no-cone /platform/Device/SiliconLabs/EFR32FG14P/ \
      /platform/CMSIS/Core/Include/ /platform/common/inc/ /platform/emlib/
  ```

  In Git Bash prefix the last command with `MSYS_NO_PATHCONV=1`, otherwise the leading `/` is
  rewritten to the Git install path.
- pyOCD with the Silicon Labs DFP pack (once): `pyocd pack install EFR32FG14P231F256GM48`.
- A CMSIS-DAP probe (Raspberry Pi Debug Probe) on the back pads `CLK`/`DIO`/`GND`; leave `RES`
  and `SW` unconnected. Supply 3.0 V on `BAT+` with a current limit of at least 0.5 A (the board
  peaks around 330 mA).

## Build & flash

```bash
make GECKO_SDK=C:/Tools/gecko_sdk_4.4.6          # build/smoke.{elf,hex,bin}
make GECKO_SDK=C:/Tools/gecko_sdk_4.4.6 flash    # tools/flash.py via pyOCD
```

On Windows pass `GECKO_SDK` as a `C:/...` path (the Windows toolchain does not understand
`/c/...`). The build is warning-free (`-Wall -Wextra -Werror` for project sources).

The chip must be unlocked first. A factory GW-SD3 is debug-locked (AP0 IDR `0x26E60011`); the
Series 1 `AAP DEVICE_ERASE` unlocks it but **permanently erases the stock application** (the
external `IC6` flash holds only sound data). See [../README.md](../README.md).

## Verify

Read `g_heartbeat` (address from `arm-none-eabi-nm build/smoke.elf`) twice over SWD: it must
increase by 2 per second. Bench result 2026-11-26 on Unit 1: flash content equal to the image,
`+6` in 3 s, `PF4` in push-pull mode.
