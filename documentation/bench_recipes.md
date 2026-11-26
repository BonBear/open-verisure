# Bench recipes for the smoke detector, voicebox and siren

Step-by-step procedures and pin references for the devices in
[device_survey.md](device_survey.md), limited to the tools on the bench: Raspberry Pi Debug Probe
(CMSIS-DAP, SWD, 3.3 V), CJMCU FT232H (MPSSE SPI, 3.3 V IO), the USB-serial adapter (TXD, RXD, DTR,
CTS: **no RTS**), Saleae Logic 4, DMM, PCBite probes, USB microscope, the 868 MHz receiver, the
project gateway, the PS3005D and the Otii Arc. Results go in each device's `re_worksheet.md`.

Pin numbers marked "unverified" come from reference schematics or secondary sources; check them
against the datasheet before relying on them, and confirm every net by continuity.

## Read this first: what can erase the only unit

| Device | Action | What happens |
| :--- | :--- | :--- |
| Smoke detector | Debug unlock (Simplicity Commander "unlock", J-Link unlock, AAP `DEVICE_ERASE`) | Mass-erases the EFR32 flash: the firmware is gone |
| Smoke detector | `efm32 debuglock` in OpenOCD | **Locks** the part. Never run it |
| Siren, voicebox | MSP430 BSL with a wrong password | Mass-erases the MSP430 flash on most parts (TI SLAU319). The password is the 32-byte interrupt vector table, which is unknown without the firmware, so **any BSL attempt is in practice an erase** |
| Siren, voicebox | MSP430 JTAG security fuse already blown | No JTAG read is possible; do not try BSL as a way round it (see above) |
| Voicebox | 3.3 V on the W25Q16DW | It is a 1.8 V part: may damage it or the DCX81 |
| All | Shorting the battery / reversed supply | The smoke detector's battery is soldered in and not replaceable |

Safe now: SWD **read-only** on the smoke detector, SPI flash dumps (with the 1.8 V precaution for the
voicebox), Saleae sniffing of MCU-to-radio SPI, and listening to the radio with the gateway or the
receiver. **MSP430 flash cannot be read safely with an off-the-shelf tool on the current bench**:
the siren's `MSP430F149` supports **4-wire JTAG only** (no Spy-Bi-Wire), so a LaunchPad eZ-FET
(SBW-only) will **not** work on it — reading it requires a 4-wire JTAG probe (TI `MSP-FET`, legacy
`MSP-FET430UIF`, Olimex `MSP430-JTAG-TINY-V2`, or a custom ESP32/SLAU320 4-wire JTAG replicator)
and an intact JTAG fuse. The voicebox's `MSP430F5435A` supports both 4-wire JTAG and 2-wire SBW
(so a LaunchPad eZ-FET works there). The FT232H has no maintained MSP430 JTAG tool, though its
UART mode (`TXD`/`RXD`/`DTR`/`RTS`) can flash custom firmware via ROM BSL once the original flash
is no longer needed.

## 1. Smoke detector: EFR32FG14 over SWD (Pi Debug Probe + OpenOCD)

### Pins (EFR32FG14, QFN48)

From the Silicon Labs BRD4262B reference schematic (EFR32FG14P233F256GM48, pin compatible within
FG14): **secondary source, confirm against the datasheet pin table.**

| Signal | GPIO | QFN48 pin |
| :--- | :--- | :--- |
| SWCLK | PF0 | 1 |
| SWDIO | PF1 | 2 |
| SWO | PF2 | 3 |
| RESETn | dedicated | 12 |
| HFXO (HFXTAL_N / _P) | | 10 / 11 |
| LFXO (LFXTAL_N / _P) | PB14 / PB15 | 35 / 36 |
| AVDD | | 34 |
| VREGVDD | | 39 |
| DVDD | | 40 |
| DECOUPLE (digital LDO output, do not load) | | 41 |
| IOVDD | | 42 |
| GND | exposed pad | 49 |

Series 1 parts have no fixed ROM UART bootloader: the `TXD`/`RXD` pads go to whatever USART the
Verisure firmware uses.

Part number: `EFR32FG14P231F<flash>GM48`. `F128` = 128 KB flash / 16 KB RAM, `F256` = 256 KB / 32 KB;
both exist for P231. Feature code 231 is, per the Silicon Labs device pages (unverified),
**sub-GHz only** (233 is dual-band), so the board probably has no 2.4 GHz radio in use. The DI page
read below gives the real part number.

### Wiring

| Debug Probe (SWD connector) | Smoke detector pad |
| :--- | :--- |
| SWCLK | `CLK` |
| SWDIO | `DIO` |
| GND | GND (find by continuity: there is no GND pad in the 8-pad group) |
| (optional) nRESET via a GPIO / manual | `RES` |

The probe does not power the target. Run the detector on its own battery, or remove the battery
and feed `VCC` from the PS3005D at the battery voltage with a low current limit. Leave `SW` unconnected
until its function is known (it may be SWO).

### Commands (read-only)

```
openocd -f interface/cmsis-dap.cfg -c "transport select swd" -f target/efm32.cfg
```

OpenOCD's `efm32` driver knows the EFR32FG14 (family IDs 61-63). On connect it reads the DI page and
prints the part number, flash and RAM size: pure reads, no erase. In a telnet session
(`telnet localhost 4444`):

```
flash info 0
dump_image smoke_flash.bin 0x0 0x40000
```

Use `0x20000` for a 128 KB part. Record the SHA-256 of the dump in the worksheet.

How the lock state shows up:

- **Unlocked**: OpenOCD connects, `flash info 0` works, and the Debug Lock Word (word 127 of the lock
  bits page) reads `0xFFFFFFFF`.
- **Locked**: the normal AHB-AP does not answer; only the Authentication Access Port (AAP) does.
  **Stop here.** The only way in is an AAP device erase, which wipes the firmware. Try the external
  W25Q16JL instead, and decide later.

Never run `efm32 debuglock`, `flash erase_*`, `flash write_*` or `program` on the only unit.

## 2. SPI flash dumps (FT232H + flashrom)

### Pins (W25Q16JL / W25Q16DW, SOIC-8)

| Pin | Name | FT232H (ADBUS) |
| :--- | :--- | :--- |
| 1 | CS# | AD3 |
| 2 | DO (MISO) | AD2 |
| 3 | WP# | tie to VCC |
| 4 | GND | GND |
| 5 | DI (MOSI) | AD1 |
| 6 | CLK | AD0 |
| 7 | HOLD# / RESET# | tie to VCC |
| 8 | VCC | see below |

| Part | Device | VCC | JEDEC ID (9Fh) |
| :--- | :--- | :--- | :--- |
| W25Q16JL | Smoke detector | ~2.3–3.6 V (unverified) | probably `EF 40 15` (unverified) |
| W25Q16DW | Voicebox | **1.65–1.95 V** | `EF 60 15` (Winbond datasheet) |

### Command

```
flashrom -p ft2232_spi:type=232H,port=A -r dump.bin
```

Read twice and compare the SHA-256 of both files before trusting the dump.

### In-circuit cautions

- Reading in circuit powers the rest of the board through the flash's VCC and SPI lines, and the
  host MCU may drive the bus at the same time. Hold the MCU in reset (EFR32: `RES` low; MSP430:
  RST low) or lift the flash.
- **Voicebox (W25Q16DW, 1.8 V):** do not connect the 3.3 V FT232H directly. Either feed the FT232H's
  VCCIO with 1.8 V (the CJMCU board usually ties VCCIO to its 3.3 V regulator, so this needs a
  board modification), or power the flash side at 1.8 V and put a 4-channel bidirectional level
  shifter (TXB0104 / TXS0108) on CLK, MOSI, CS and MISO. MISO at 1.8 V is marginal for the FT232H's
  input threshold, so shift it too.
- Smoke detector (W25Q16JL): can be read at 3.3 V, after confirming its VCC pin voltage on the board.

## 3. Siren and voicebox: MSP430

### MSP430F149 (siren), 64-pin LQFP (TI SLAS272)

| Signal | Pin |
| :--- | :--- |
| DVCC | 1 |
| P1.1 / BSL TXD | 13 |
| P2.2 / BSL RXD | 22 |
| TDO/TDI | 54 |
| TDI/TCLK | 55 |
| TMS | 56 |
| TCK | 57 |
| RST/NMI | 58 |
| AVSS | 62 |
| DVSS | 63 |
| AVCC | 64 |
| XIN/XOUT, XT2IN/XT2OUT | unverified: find by continuity to the crystals |

No TEST pin: JTAG is always 4-wire until the fuse is blown. BSL entry would be RST + TCK. Do not
use it (see the table at the top).

### MSP430F5435A (voicebox), 80-pin LQFP (TI SLAS655H)

PN pin numbers verified against SLAS655H figure 7-2 and the terminal table. TEST, PJ.0–PJ.3 and
RST/NMI sit next to each other (pins 71–76) on the pin 61–80 edge.

| Signal | PN pin | Function |
| :--- | :--- | :--- |
| TEST/SBWTCK | 71 | JTAG enable / Spy-Bi-Wire clock |
| PJ.0/TDO, PJ.1/TDI/TCLK, PJ.2/TMS, PJ.3/TCK | 72, 73, 74, 75 | 4-wire JTAG |
| RST/NMI/SBWTDIO | 76 | Reset / Spy-Bi-Wire data |
| P1.1 / P1.2 | 18 / 19 | BSL TXD / RXD (UART BSL; F543xA password = vector table) |
| P7.0/XIN, P7.1/XOUT | 13, 14 | XT1 (32.768 kHz) |
| P5.2/XT2IN, P5.3/XT2OUT | 69, 70 | XT2 |
| DVCC1, DVCC2, DVCC3, DVCC4 | 16, 51, 31, 67 | digital supply |
| DVSS1, DVSS2, DVSS3, DVSS4 | 15, 50, 30, 68 | digital ground |
| AVCC, AVSS | 11, 12 | analog supply / ground |
| VCORE | 49 | core regulator output (470 nF only, never load) |

The F5xx JTAG lock is an electronic fuse (a lock signature in flash), not a physical fuse. SBW needs
only pins 71 and 76 plus DVCC/GND.

### What to do now

1. Map the candidate pads to the pins above with the DMM (worksheets section "debug access").
2. Do **not** run BSL or blind JTAG commands.
3. Reading the MSP430 flash needs a compatible JTAG probe and an intact fuse:
   - **Siren (`MSP430F149`, 4-wire JTAG only, no SBW):** TI `MSP-FET`, `MSP-FET430UIF`, Olimex
     `MSP430-JTAG-TINY-V2`, or a custom ESP32 running TI SLAU320 4-wire JTAG. A LaunchPad eZ-FET
     (SBW-only) does **not** support the F149.
   - **Voicebox (`MSP430F5435A`, JTAG + SBW):** any of the above, or a LaunchPad eZ-FET over SBW.
   With a JTAG probe, first read the device ID and the fuse state only.
4. The standalone USB-serial adapter has no RTS line, so it cannot drive BSL entry; the CJMCU
   FT232H in UART mode has both DTR and RTS and can be used for BSL flashing of custom firmware
   later (never for reading: wrong password mass-erases).

## 4. Radio SPI sniffing (Saleae Logic 4)

| Channel | Siren (CC1101) | Voicebox (Si4460) |
| :--- | :--- | :--- |
| CH0 | SCLK (CC1101 pin 1) | SCLK |
| CH1 | MOSI = SI (pin 20) | SDI |
| CH2 | MISO = SO/GDO1 (pin 2) | SDO |
| CH3 | CSn (pin 7) | nSEL |

The Logic 4 samples at up to 12 MS/s, which is enough for an SPI clock up to about 3 MHz. Check SCLK
with the scope first. Start the capture before powering the board, so it catches the whole register
setup.

Decode the siren capture with the project's tool. Export the SPI analyser result from Logic 2 as
CSV, then:

```
python tools/cc1101_spi_decode.py capture.csv --xtal-mhz 27
```

Use `--xtal-mhz 26` if XT1 turns out to be 26 MHz. The tool prints the strobes, the final register
image and the derived carrier, data rate, deviation, sync word and WOR timing. The Si4460 uses a
command-based API, not registers; the tool does not decode it (yet).

### Pins (CC1101, QFN20, TI SWRS061)

| Pin | Name | Pin | Name |
| :--- | :--- | :--- | :--- |
| 1 | SCLK | 11 | AVDD |
| 2 | SO (GDO1) | 12 | RF_P |
| 3 | GDO2 | 13 | RF_N |
| 4 | DVDD | 14 | AVDD |
| 5 | DCOUPL (do not load) | 15 | AVDD |
| 6 | GDO0 | 16 | GND |
| 7 | CSn | 17 | RBIAS |
| 8 | XOSC_Q1 | 18 | DGUARD |
| 9 | AVDD | 19 | GND |
| 10 | XOSC_Q2 | 20 | SI |

### Pins (Si4460, QFN20): indicative

The numbers depend on the datasheet revision; trace by function.

| Pin | Name | Pin | Name |
| :--- | :--- | :--- | :--- |
| 1 | SDN (shutdown, active high) | 11 | nIRQ |
| 2 / 3 | RXp / RXn | 12 | SCLK |
| 4 or 5 | TX | 13 | SDO |
| 7–8 | VDD | 14 | SDI |
| 9 / 10 | GPIO0 / GPIO1 | 15 | nSEL |
| 16 / 17 | XOUT / XIN | 19 / 20 | GPIO2 / GPIO3 |

Silicon Labs AN785 recommends a 26 MHz crystal for 868 MHz designs (the family default is
30 MHz): expect 26 MHz on the voicebox and measure it.

## 5. Listening to the radio

1. Flash the gateway's evaluation build (`pio run -d 868Receiver -e lolin_s2_mini_eval -t upload`)
   and run `sniff on` (rejected and bad-CRC frames) or `sniff all` on the serial console. Then
   trigger the device (test button, tamper, alarm from the hub).
2. The gateway only sees frames with the Verisure parameters: 868.950 MHz, 2-FSK, 38.4 kBaud, sync
   `D3 91`, taken as 58 bytes. If nothing shows up, find the device with the 868 MHz receiver first
   (frequency, modulation, rate), then compare.
3. Record what you find in the worksheet's radio section.

## 6. Gateway sniff command (eval build)

Bench test plan for the `sniff [on|off|all|unknown]` CLI on the evaluation build (`RADIO_EVAL=1`,
branch `feat/gateway-sniff` / PR #47):

1. **Flash the eval build:**
   ```
   pio run -d 868Receiver -e lolin_s2_mini_eval -t upload --upload-port COM9
   ```
2. **Commands & expected output:**
   - `sniff` -> `[CLI] Sniff mode: off` (default at boot; not stored in NVS).
   - `sniff on` or `sniff unknown` -> `[CLI] Sniff mode: unknown (rejected or bad-CRC frames)`.
     Trigger an unpaired/bad-CRC transmission or tamper on a non-paired device: prints
     `[Sniff#<module>] ch=<ch> len=<n> crc56=<ok|bad> crc24=<ok|bad> rssi=<dBm> lqi=<n> fe=<kHz> <hex>`
     only for rejected (`[Security]`) or invalid-CRC frames, not for valid paired sensors.
   - `sniff all` -> `[CLI] Sniff mode: all (every frame)`. Every non-CAL frame on module #1 (`868.950 MHz`)
     and #2 (`869.525 MHz` PCH) is dumped *after* packet handling and any camera poll answer.
   - `sniff off` -> `[CLI] Sniff mode: off`. Stops `[Sniff#n]` lines immediately.
   - Invalid argument (e.g. `sniff foo`) -> `[CLI] Usage: sniff [on|off|all|unknown]`.
3. **Pass criteria:**
   - **Rate limit (20 lines/s):** Under `sniff all` during a burst (>20 frames/s, e.g. camera picture
     chunks), at most 20 `[Sniff#n]` lines print per 1000 ms window, and
     `[Sniff] <N> line(s) suppressed (rate limit 20/s)` appears once at the start of the next window.
   - **Poll deadline preserved:** Under `sniff all`, camera polls (`0xBA`) still receive timely
     answers (`radio` diagnostics `skipped late` remains 0; `all` dump runs after `answerCameraPoll`).
   - **`off` stops output:** Zero `[Sniff#n]` lines appear after `sniff off` (and after `reboot`).
4. **Reflash the release build** when finished so `RADIO_EVAL` commands are not left active:
   ```
   pio run -d 868Receiver -e lolin_s2_mini -t upload --upload-port COM9
   ```

## Sources

TI SWRS061 (CC1101), SLAS272 (MSP430F149), SLAS655H (MSP430F5435A), SLAU319 (MSP430 BSL); Silicon
Labs Si4464/63/61/60 datasheet, AN785, AN0002.1, BRD4262B schematic; Winbond W25Q16DW datasheet;
OpenOCD `efm32` driver source; flashrom `ft2232_spi` documentation; Saleae Logic 4 data sheet.
