# Survey of other Verisure devices

Photo-based hardware survey of three Verisure devices that the project does not support yet, made
before any bench work. Each device has a README (what the photos show) and a worksheet to fill in on
the bench.

| | Smoke detector GW-SD3 | Voicebox GW-AUD-VB | Siren |
| :--- | :--- | :--- | :--- |
| Folder | [SMOKE_DETECTOR](../SMOKE_DETECTOR/README.md) | [VOICEBOX](../VOICEBOX/README.md) | [SIREN](../SIREN/README.md) |
| Worksheet | [re_worksheet.md](../SMOKE_DETECTOR/re_worksheet.md) | [re_worksheet.md](../VOICEBOX/re_worksheet.md) | [re_worksheet.md](../SIREN/re_worksheet.md) |
| Maker | Made in Japan (PCB K2226) | Panasonic, Japan (PCB K1969-1) | Japanese silkscreen (PCB M1765-1) |
| MCU | Silicon Labs EFR32FG14P231 (Cortex-M4) | TI MSP430F5435A | TI MSP430F149 |
| Sub-GHz radio | integrated in the EFR32 | Silicon Labs Si4460 (assumed) | TI CC1101 |
| Other radio | none expected (P231 = sub-GHz only, to confirm) | DSP Group DCX81 (DECT voice SoC) | none |
| External flash | Winbond W25Q16JL (16 Mbit) | Winbond W25Q16DW (16 Mbit, 1.8 V) | none |
| Audio | 8 Ω speaker, driver unknown (IC81?) | 2x 8 Ω speakers, MEMS microphone | OKI speech LSI + horn driver |
| Other sensors | optical smoke chamber | PIR | thermistor, tamper switch |
| Debug | labelled pads: CLK, DIO, SW, RES, TXD, RXD, TPR, FIRE (SWD + UART) | unlabelled pogo cluster (JTAG/SBW to find) | TP pads (JTAG to find) |
| Bench recipes | [bench_recipes.md](bench_recipes.md) §1, §2 | §2, §3, §4 | §3, §4 |
| Battery | built in, not replaceable | 3x lithium, 3 V | to check |

## What the photos already tell

- None of the three uses the CC1110 SoC of the door sensor and the camera. The siren uses the same
  CC1101 as our gateway. The voicebox and the smoke detector use Silicon Labs radios. All three
  probably share the 868.950 MHz link, but that has to be confirmed by listening.
- The smoke detector is the easiest place to start: its SWD and UART pads are labelled, and it has a
  standard Cortex-M debug port.
- The MSP430 boards (siren, voicebox) need the JTAG pads found by continuity first. Reading their
  flash needs a TI FET (MSP-FET, or the eZ-FET on an MSP430 LaunchPad), which the bench does not
  have. Their BSL is not an option: a wrong password mass-erases the flash. Until a FET is there,
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
