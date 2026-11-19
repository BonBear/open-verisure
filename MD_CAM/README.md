# Verisure motion camera (GW MD CAM2)

Open-source firmware for the Verisure **GW MD CAM2** PIR motion camera. When the alarm is armed
and the PIR sees motion, the camera takes pictures and sends them over 868 MHz to the
[gateway](../868Receiver/README.md), which publishes them to Home Assistant as a camera entity.
In the dark it takes grayscale pictures lit by its infrared flash.

**Converting a factory camera** needs both processors programmed once, in this order:

1. The CC1110 over its debug header J6 with the ESP_CC_Flasher
   ([CC1110 firmware](#cc1110-firmware-firmware)). A factory CC1110 is debug-locked, so this needs
   `--erase`, which wipes the factory firmware for good (it cannot be read back). It must come
   first: the Blackfin is only powered long enough for JTAG with `HOLD`, which only this
   project's CC1110 firmware handles.
2. The Blackfin's boot flash over JTAG (header J5, an FT232H): back it up first
   (`jtag/bf_read_flash.sh`), then write `snap_link` and the boot selector
   ([Blackfin program](#blackfin-program-blackfin)).

The factory image program does not speak this firmware's link protocol, so flashing only the
CC1110 is not enough.

## Hardware

| Part | Role |
| :--- | :--- |
| **TI CC1110F32** | Radio (868.950 MHz), PIR input, power switching, alarm logic. Programmed through its debug header J6 like the door sensor (wiring: [hardware_cc_flasher.md](../documentation/hardware_cc_flasher.md)). |
| **ADI Blackfin BF512** | Image processing: captures from the sensor, JPEG encoding, deltas, IR flash. Boots from an EN25F20 SPI flash; programmed over JTAG (J5). |
| **OmniVision OV7955** | VGA camera sensor (TWI address 0x40), parallel data into the Blackfin PPI. |
| **IS42S16800F** | 16 MB SDRAM on the Blackfin: whole frames, delta reference, send buffer. |
| **IR flash board** (J2/J3) | Infrared LEDs on Blackfin PF10 + PF12; invisible to the eye. |
| **PIR** | Motion detector on CC1110 P1_7. |

## What it does

* **Alarm pictures:** armed + motion → a 320x240 picture every ~5 s while motion lasts (the 2nd one
  640x480), plus one every 6 hours while armed. In the dark (scene mean below 40) each picture is a
  grayscale IR flash picture instead.
* **Deltas:** while the Blackfin is on, a picture is sent as only the 16x16 blocks that changed;
  the gateway splices them into its copy of the last full picture.
* **Pictures on request** from Home Assistant or MQTT: `verisure/<id>/picture/set` with
  `D1` (full colour), `D2` (640x480), `D3` (delta), `D4` (IR flash, grayscale),
  `D5`/`D6` (as D3/D2 with light, flash picture in the dark).
* **Alarm state** from Home Assistant (`verisure/alarm/set` ON/OFF) reaches the camera with the
  answer to its next poll: every ~30 s while armed, ~120 s while disarmed and idle, and right
  after each picture.
* **Picture quality:** whole-frame capture (no motion smear), 2x2 averaging, lens shading
  correction, saturation, sharpening, JPEG Q65. Colour drift of the sensor (strong right after a
  cold start) is measured against the reference before each delta and compensated, so only real
  changes are sent.
* **Speed:** a 320x240 picture (~8 KB) arrives 5–8 s after the command, 640x480 (~18 KB) in ~12 s
  (56-byte radio packets with 40 bytes of JPEG each). Chunks the gateway missed are resent on
  request while the Blackfin still holds the picture.
* **Power:** the Blackfin (~0.17–0.19 A at 3.0 V) is powered only for pictures and 15 s after
  (one D1 picture: Blackfin on ~18.7 s, 3.1–3.4 As ≈ 0.9 mAh from 3.0 to 2.55 V); the CC1110
  sleeps in PM2 otherwise. Disarmed and idle: 0.031 mA mean at 3.0 V (Otii 2026-11-17, floor
  2 µA; a poll costs ~0.9 mAs, ≈ 8 µA at that build's poll every ~109 s, the rest is the sleep
  timer wake-ups; since the sleep tick fix of 2026-11-18 disarmed polls come every 120 s). Armed
  and idle (polls every 30 s): 0.054 mA mean, ~1.3 mAh a day (Otii overnight 2026-11-18, 5 h
  without pictures; 0.021 mA between polls). Pictures
  dominate battery life: with ~3.5 Ah usable from three CR123A, idle alone would last for years,
  but each picture costs ~0.9 mAh (~4000 pictures per set of batteries). A watchdog restarts the
  CC1110 if it ever hangs.
* **Battery flags (Otii steps, 2026-11-17):** 3.0 V: 92 %; 2.8 V: 43 %; 2.65 V: 6 % and low
  battery; 2.55 V: 0 %, low battery, still complete pictures. The value under load reads
  ~50 mV below the supply (cables and the Blackfin's current).
* **Overnight armed soak (2026-11-17/18, 7 h on the Otii, dark room):** 922 polls, none more than
  45 s apart, no reset, every picture complete. Without the flash a picture in the dark is black
  (`D1` and `D2`, scene mean ~4/255); `D4`/`D5` give usable grayscale flash pictures. `D2` never
  uses the flash, so 640x480 is for daylight only.
* **Armed day run (2026-11-18, 7.8 h on the Otii, daylight):** armed idle 0.054 mA again; the 6 h
  idle picture came after 5 h 28 min (the sleep timer ran ~9 % fast, polls ~27 s apart; fixed the same
  day, polls now 30.0 s apart); one D1 in
  daylight costs ~3.7 As (~1.0 mAh) with ~22 s above 20 mA; `test_camera_e2e.py` passed 12/12, with
  `D5`/`D6` giving colour pictures.

The protocol between the parts and over the air is described in
[docs/Camera_protocol.md](docs/Camera_protocol.md).

## Build and flash

### CC1110 firmware (`firmware/`)

Needs SDCC, Python 3 and the ESP_CC_Flasher (by USB serial, or over Wi-Fi with `--host`). Give
the camera's ID from its label with `--id` the first time; the device key is derived from the
master key in `868Receiver/src/secrets.h` and written to `firmware/config.h` (git-ignored), and
later runs take the ID from there.

```bash
python MD_CAM/build_and_flash_md_cam.py --id "2NCE KS3T" --serial <flasher port>   # COM12, /dev/ttyUSB0 on Linux
python MD_CAM/build_and_flash_md_cam.py --serial <flasher port>                    # ID from config.h
python MD_CAM/build_and_flash_md_cam.py --compile-only                            # build check only, never flash it
```

Options (`--help`): `--id`, `--serial`, `--host`, `--gateway`, `--no-seed`, `--compile-only` and
`--erase` (needed for a debug-locked factory CC1110; the chip erase wipes the factory firmware for
good). The door sensor's script has more; both use `cc1110_tools.py` in the repository root.

The script prints the master key's check value (KCV) so it can be compared with the gateway's
`status` output without showing the key. Like the door sensor's script it asks the gateway for the
camera's last packet counter first and stops if it cannot reach it: set `GATEWAY_HTTP=http://<ip>`
when `verisure-gateway.local` does not resolve, or pass `--no-seed` (counter from block 1, then
`replay reset <ID>` on the gateway).

### Blackfin program (`blackfin/`)

Needs the ADI Blackfin GCC toolchain (`bfin-elf-gcc`), an FT232H on the JTAG header J5,
`bfin-gdbproxy` in WSL and `usbipd` to attach the FT232H to WSL.

```bash
cd MD_CAM/blackfin
python build_ldr.py snap_link.c jpeg_enc.c      # -> build/snap_link.ldr (built with -O2)
python build_ldr.py --elf-only flash_write.c    # -> build/flash_write.elf, used by bf_flash.sh
python build_ldr.py --elf-only flash_read.c     # -> build/flash_read.elf, used by bf_read_flash.sh
MSYS_NO_PATHCONV=1 bash jtag/bf_flash.sh         # writes snap_link.ldr to the boot flash at 0x8000
```

The Blackfin must be powered while flashing (~90 s): publish `HOLD` to
`verisure/<id>/picture/set` first; the CC1110 then powers it and keeps it on for 10 min
without taking a picture. `jtag/jtag_common.sh`
describes the one-time WSL/usbipd setup. `jtag/bf_read.sh` reads the status words and
`jtag/bf_set.sh` writes tuning words (sharpening, lens shading, saturation, JPEG quality, delta
threshold, flash test).

The boot flash holds a small boot selector in sector 0 (`boot_select.S`, written once with
`flash_sector0.c`): it starts our program at 0x8000 when the CC1110 holds P1_2 high at power-on
(it always does). The factory firmware that used to be at 0x10000 was erased on 2026-11-16 after
it had been backed up and checked against the chip (`jtag/bf_read_flash.sh` reads the boot
flash).

Writing the selector is **not scripted**. From the sources it takes
`python build_ldr.py boot_select.S` (`build/boot_select.ldr`, at most 4 KB) and
`python build_ldr.py --elf-only flash_sector0.c`, then a gdb session like the one in
`jtag/bf_flash.sh`: load `build/flash_sector0.elf`, restore the LDR to 0xFF900000, set the
length at 0xFF800014 and the confirmation word 0x5EC70000 at 0xFF800010, detach, and check for
0xF1A55EC0 at 0xFF800000 and result 0 at 0xFF800004. It erases sector 0, the start of the factory
image program, so back up the boot flash before.

## Test

With the camera powered and the gateway on USB:

```bash
python MD_CAM/test_camera_e2e.py            # 12 checks: polls, pictures, deltas, flash, alarm
python MD_CAM/test_camera_e2e.py --gateway-restart
python MD_CAM/test_resend.py 7 D1 D2           # gateway drops every 7th chunk; checks the resend
```

## Files

| Path | Content |
| :--- | :--- |
| `firmware/main.c` | CC1110 firmware (alarm loop, radio, downlink, Blackfin power and link, PM2 sleep) |
| `blackfin/snap_link.c`, `jpeg_enc.c` | Blackfin program: capture, image processing, JPEG, deltas, flash |
| `blackfin/boot_select.S`, `flash_*.c`, `build_ldr.py`, `bf512_l1.ld` | Boot selector, flash tools, LDR build, linker layout |
| `blackfin/jtag/` | JTAG helper scripts (flash, read status, write tuning words) |
| `build_and_flash_md_cam.py` | Build and flash the CC1110 firmware |
| `dump_cc1110.py` | Read out (back up) the CC1110 flash without erasing it |
| `test_camera_e2e.py` | End-to-end test through the gateway and MQTT |
| `test_resend.py` | Resend test: the gateway drops every Nth chunk and the camera must resend it |
| `firmware/config.h.example` | Template for the generated, git-ignored `firmware/config.h` |
| `docs/` | Protocol, measurements and reverse-engineering notes and images |

## Docs

* [docs/Camera_protocol.md](docs/Camera_protocol.md): the CC1110–Blackfin link and the radio protocol (current).
* [docs/Hardware_pinout_and_measurements.md](docs/Hardware_pinout_and_measurements.md): CC1110 pinout, debug header J6, LED board and flasher wiring.
* [docs/Measurements_PIR_header.md](docs/Measurements_PIR_header.md): the PIR board header and the idle current fix (2026-11-16).
* [docs/Blackfin_boot_and_JTAG.md](docs/Blackfin_boot_and_JTAG.md): historical log: Blackfin boot mode, JTAG, boot flash and standalone boot.
* [docs/Camera_J1_measurement_plan.md](docs/Camera_J1_measurement_plan.md): historical log: camera module J1, sensor ID and the first pictures.
* [docs/Measurements_phase2_power_and_buses.md](docs/Measurements_phase2_power_and_buses.md): historical log: early power-enable and bus measurements.

Photos are in [docs/images/](docs/images/): the main board is in `camera_board_front.jpg`,
`camera_board_back.jpg` and `camera_board_front_marked.jpg`; picture samples from the camera are
`camera_*_320x240.*`.

Open work, including what needs someone at the bench, is listed in [TODO.md](../TODO.md).
