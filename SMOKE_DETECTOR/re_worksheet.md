# GW-SD3 smoke detector: bench worksheet

Fill in on the bench. Leave a cell empty when it has not been measured; write `n/a` when it does
not apply. Background and part list: [README.md](README.md).

Unit used: ☐ 3EA8VKXK ☐ 3EB7KFZQ  Date: ________  Tools: ______________________

Safety: the battery is soldered in or plugged in and cannot be replaced. Do not short it, and
keep the probe away from the battery terminals when the board is loose. The chamber is optical (no
radioactive source).

## 1. Power

| Point | Expected | Measured | Notes |
| :--- | :--- | :--- | :--- |
| Battery voltage (open circuit) | 3.0–3.6 V (lithium) | | |
| Battery type / marking (inside the base) | | | |
| `VCC` pad to GND | ~3 V or ~1.8 V | | |
| `VCA` pad to GND | analog supply? | | |
| EFR32 DECOUPLE pin / DVDD | ~1.2 V / ~1.8 V | | |
| Boost/driver rail at the 100 µF `107 6.38` cap | > battery when sounding? | | |
| Idle current (supervision) | µA range | | |
| Current while transmitting (peak) | 10–30 mA | | |
| Current while sounding | | | |
| Where GND can be reached (pad / part pin) | | | |

## 2. Clocks

| Part | Marking | Expected | Measured (frequency counter / scope) |
| :--- | :--- | :--- | :--- |
| HFXO next to the EFR32 | `3840T` | 38.400 MHz | |
| LFXO (brown 2-pad) | `A320L` | 32.768 kHz | |

## 3. Debug header (8 pogo pads on the back)

Measure with the board powered by its own battery unless noted. "Idle level" = DC voltage to GND.
"Goes to" = EFR32 pin number or part found by continuity (beeper with the battery disconnected).

| Pad | Assumed | Idle level (V) | Goes to (EFR32 pin / part) | Confirmed function |
| :--- | :--- | :--- | :--- | :--- |
| CLK | SWCLK | | | |
| DIO | SWDIO | | | |
| SW | SWO or switch | | | |
| RES | RESETn | | | |
| TXD | UART TX | | | |
| RXD | UART RX | | | |
| TPR | tamper | | | |
| FIRE | alarm signal | | | |

Pad pitch / diameter (for a pogo jig): pitch ______ mm, pad Ø ______ mm.
Position of the jig holes in the base relative to the pads: ______________________

### SWD connection test

| Step | Result |
| :--- | :--- |
| Probe used (J-Link / CMSIS-DAP / other) | |
| Target voltage seen by the probe | |
| `IDCODE` / DPIDR read | |
| Part number read (DI page) | |
| Flash size reported | |
| Debug lock state (locked / unlocked) | |
| Main flash dump possible? (file name, size, SHA-256) | |
| **Do not run a device erase on the only unit** | ☐ read and understood |

### UART test (TXD/RXD)

| Item | Result |
| :--- | :--- |
| Activity on TXD at power-up / button press (scope) | |
| Baud rate (from the bit width) | |
| Text or binary? First bytes seen | |
| Reaction to input on RXD | |

## 4. External SPI flash (IC6, W25Q16JL)

| Item | Result |
| :--- | :--- |
| VCC at pin 8 | |
| JEDEC ID (should be EF 40 15 or EF 60 15) | |
| Dumped in circuit (clip) or desoldered? | |
| Dump file name, size, SHA-256 | |
| Content (empty / code / audio / log; `binwalk`, strings) | |
| EFR32 pins wired to CS / CLK / MOSI / MISO | |

## 5. Unknown ICs

| Ref | Marking | Package / pin count | Supply pin and voltage | Nets / connected parts | Identified as |
| :--- | :--- | :--- | :--- | :--- | :--- |
| IC81 | `C4660 224KU` | TSSOP-20 | | | |
| IC301 | `304A 029J 363` | QFN-32 | | | |
| 6-pad near U.FL | `341 ZF` | | | | |
| SOT-23 parts `YK`, `KFD`, `A1`, `4A` | | | | | |

## 6. Connectors and peripherals

| Item | Pins | Measured / traced | Function |
| :--- | :--- | :--- | :--- |
| CN301 (3-pin) | | | |
| 2-pin connector (back, top) | | | battery? |
| Speaker (8 Ω): driven by | | | |
| LED41 / LED42 / LED43 colour and EFR32 pin | | | |
| Test button: EFR32 pin | | | |
| Tamper (bracket) detection: switch or reed? | | | |
| Optical chamber: IR LED drive pin, photodiode input | | | |
| U.FL connector: connected to the antenna path? | | | |

## 7. Numbered test pads

Add rows as needed.

| Pad | Side | Idle voltage | Connected to | Notes |
| :--- | :--- | :--- | :--- | :--- |
| 15 | front | | | next to the EFR32 |
| 23 | front | | | |
| 28 | front | | | |
| 32 | front | | | |
| 39 | front | | | next to IC81 |
| 40 | front | | | |
| 48/49/56/57/58/59 | front | | | 2x4 grid next to IC6 (SPI?) |
| 50 | front | | | |
| 62 | front | | | |
| 90/91/94 | front | | | near CN11 |
| 95 | front | | | |
| 61 | back | | | next to the header |
| 64 | back | | | next to FIRE |
| 10 | back | | | next to R1 |

## 8. Radio

| Item | Result |
| :--- | :--- |
| Frequency seen on the SDR (test button / alarm / supervision) | |
| Modulation, data rate, deviation | |
| Seen by the gateway with the door sensor settings (868.950 MHz, 38.4 kBaud, sync `D3 91`)? | |
| Packet length, first bytes (is the device ID in plaintext like the door sensor?) | |
| Supervision/heartbeat interval | |
| Does it receive (siren/alarm command from the hub)? | |
| 2.4 GHz activity? | |

## 9. Open questions

- [ ] What is IC81, and is the speaker driven by it?
- [ ] What is IC301?
- [ ] What does the W25Q16 hold?
- [ ] Is SWD locked?
- [ ] Same 868 MHz PHY and packet format as the door sensor?
