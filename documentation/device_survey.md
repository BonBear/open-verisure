# Survey of other Verisure devices

Hardware survey of three additional Verisure devices. The **Siren (`GW-SI`)** now has working
OpenVerisure firmware ([SIREN/firmware/README.md](../SIREN/firmware/README.md)); the smoke detector
and voicebox remain research-only. Each device has a README and a bench worksheet.

| | Smoke detector GW-SD3 | Voicebox GW-AUD-VB | Siren GW-SI |
| :--- | :--- | :--- | :--- |
| Folder | [SMOKE_DETECTOR](../SMOKE_DETECTOR/README.md) | [VOICEBOX](../VOICEBOX/README.md) | [SIREN](../SIREN/README.md) |
| Worksheet | [re_worksheet.md](../SMOKE_DETECTOR/re_worksheet.md) | [re_worksheet.md](../VOICEBOX/re_worksheet.md) | [re_worksheet.md](../SIREN/re_worksheet.md) |
| Maker | Made in Japan (PCB K2226) | Panasonic, Japan (PCB K1969-1) | Panasonic, Japan (PCB M1765-1) |
| MCU | Silicon Labs EFR32FG14P231 (Cortex-M4) | TI MSP430F5435A | TI MSP430F149 Rev S |
| Sub-GHz radio | integrated in the EFR32 | Silicon Labs Si4460 (assumed) | TI CC1101 (26 MHz XT1) |
| Other radio | none expected (P231 = sub-GHz only, to confirm) | DSP Group DCX81 (DECT voice SoC) | none |
| External flash | Winbond W25Q16JL (16 Mbit) | Winbond W25Q16DW (16 Mbit, 1.8 V) | none |
| Audio | 8 Ω speaker, driver unknown (IC81?) | 2x 8 Ω speakers, MEMS microphone | OKI L22901-707 + IC5/IC9 H-bridge piezo horn (`CN2`) |
| Other sensors | optical smoke chamber | PIR | thermistor (`TH1`), tamper switch (`SW1`) |
| Debug | labelled pads: CLK, DIO, SW, RES, TXD, RXD, TPR, FIRE (SWD + UART) | unlabelled pogo cluster (JTAG/SBW to find) | JTAG fuse blown (`TP61–64`/`TP5`); flashed via ROM BSL (`tools/esp32_msp430_bsl/`) |
| Bench recipes | [bench_recipes.md](bench_recipes.md) §1, §2 | §2, §3, §4 | §3, §4 (firmware working) |
| Battery | built in, not replaceable | 3x lithium, 3 V | 3x parallel `CR-2/3AZ` 3 V (`CN5`–`CN7`) |

## What the photos already tell

- None of the three uses the CC1110 SoC of the door sensor and the camera. The siren uses the same
  CC1101 as our gateway. The voicebox and the smoke detector use Silicon Labs radios. All three
  probably share the 868.950 MHz link, but that has to be confirmed by listening.
- The smoke detector is the easiest place to start: its SWD and UART pads are labelled, and it has a
  standard Cortex-M debug port.
- The MSP430 boards (siren, voicebox) need the JTAG pads found by continuity first. The siren's
  `MSP430F149` supports **4-wire JTAG only** (no SBW, so a LaunchPad eZ-FET will not work on it —
  it needs a 4-wire JTAG probe such as TI `MSP-FET` / Olimex `MSP430-JTAG-TINY-V2`, or a custom
  ESP32 SLAU320 replicator). The voicebox's `MSP430F5435A` also supports SBW (LaunchPad eZ-FET).
  Their BSL is not a read path: a wrong password mass-erases the flash. Until JTAG access is ready,
  stick to SPI sniffing, the external flash and the radio.
- Procedures, pinouts and the list of actions that can erase a unit are in
  [bench_recipes.md](bench_recipes.md).

## Suggested bench order

1. Smoke detector: power, GND, SWD connection test, lock state. Then listen with the gateway and an
   SDR while pressing the test button.
2. Siren: crystal frequency (26 or 27 MHz), sniff the MSP430–CC1101 SPI at power-up to get the
   register setup, then capture the hub's trigger command.
3. Voicebox: find the MSP430 JTAG pads, dump the W25Q16DW (1.8 V), and check whether the DCX81 is
   used as a DECT radio.

## Photo requests

Parts that cannot be identified from the current photos are listed as a checklist in each README
under "Photo requests": [smoke detector](../SMOKE_DETECTOR/README.md#photo-requests),
[voicebox](../VOICEBOX/README.md#photo-requests), [siren](../SIREN/README.md#photo-requests).
Take the new photos, put them in the device's folder, tick the item and the survey is updated from
them.

How to get a readable chip marking:

- Shoot straight on, close, with the phone's macro mode or 2x zoom rather than digital zoom.
- Light from a low angle (a torch held to the side): laser-etched markings show up in raking light,
  while direct light or the flash only gives glare.
- One part per photo, in focus; take two or three with the light from different sides.
- Put a ruler or calliper in the picture when a size or a pitch matters.
- No need to strip metadata yourself: it is removed (and the photos downscaled) before they are
  committed.

Photos of the new devices were stripped of GPS/EXIF metadata and downscaled to 3264 px before they
were committed.
