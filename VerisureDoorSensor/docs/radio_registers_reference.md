# Radio Registers & Clock Configuration (CC1110 / CC1101)

Both the CC1110 transmitter and the ESP32-C3 receiver (CC1101) use exactly the same radio parameters in the 868 MHz band (within the SAW filter's 868–870 MHz passband).

---

## 1. System Clock on the CC1110 (26 MHz XOSC)

The radio's PLL/frequency synthesizer requires the external 26 MHz crystal (`X1`) to be selected as the system clock:

```c
void clock_init(void) {
    SLEEP &= ~0x04;          // Power up both oscillators (OSC_PD = 0)
    while (!(SLEEP & 0x40)); // Wait until the 26 MHz XOSC is stable (XOSC_STB = 1)
    CLKCON &= ~0x47;         // Select 26 MHz XOSC (OSC = 0) and 26 MHz CLKSPD/TICKSPD
    while (CLKCON & 0x40);   // Confirm that the clock switch has taken effect
    SLEEP |= 0x04;           // Power down the 13 MHz RC oscillator
}
```

---

## 2. General Radio Parameters

* **Center frequency:** 868.950 MHz (with a 26.000 MHz crystal)
* **Data rate:** 38.383 kBaud ($\approx 38.4\text{ kBaud}$)
* **Modulation:** 2-FSK
* **Deviation:** $\approx 20.6\text{ kHz}$
* **Sync word:** `0xD3`, `0x91` (30/32-bit sync word match)
* **Packet format:** Fixed length (8-byte payload: `"HB:ALIVE"`) + hardware CRC enabled
* **Output power:** `+10 dBm` (`0xC2` at `0xDF2E`)

---

## 3. Complete Register Table

| Register | CC1101 SPI address | CC1110 XDATA address | Value | Description |
| :--- | :--- | :--- | :--- | :--- |
| `SYNC1` | `0x04` | `0xDF00` | `0xD3` | Sync word, high byte |
| `SYNC0` | `0x05` | `0xDF01` | `0x91` | Sync word, low byte |
| `PKTLEN` | `0x06` | `0xDF02` | `0x08` | Fixed packet length = 8 bytes |
| `PKTCTRL1` | `0x07` | `0xDF03` | `0x04` | `APPEND_STATUS = 1` (RSSI + LQI/CRC) |
| `PKTCTRL0` | `0x08` | `0xDF04` | `0x04` | Fixed packet length (`00`), `CRC_EN = 1` (`0x22` = CW test mode) |
| `CHANNR` | `0x0A` | `0xDF06` | `0x00` | Channel number 0 |
| `FSCTRL1` | `0x0B` | `0xDF07` | `0x06` | IF frequency (152.3 kHz) |
| `FSCTRL0` | `0x0C` | `0xDF08` | `0x00` | Frequency offset |
| `FREQ2` | `0x0D` | `0xDF09` | `0x21` | Frequency word high byte (868.950 MHz) |
| `FREQ1` | `0x0E` | `0xDF0A` | `0x6B` | Frequency word middle byte |
| `FREQ0` | `0x0F` | `0xDF0B` | `0xD1` | Frequency word low byte |
| `MDMCFG4` | `0x10` | `0xDF0C` | `0x8A` | RX filter 203 kHz (`0x5A` = 325 kHz), `DRATE_E = 10` |
| `MDMCFG3` | `0x11` | `0xDF0D` | `0x83` | `DRATE_M = 131` (38.383 kBaud) |
| `MDMCFG2` | `0x12` | `0xDF0E` | `0x03` | 2-FSK, 30/32-bit sync word (`0x00` during CW test) |
| `MDMCFG1` | `0x13` | `0xDF0F` | `0x22` | 4 bytes preamble |
| `MDMCFG0` | `0x14` | `0xDF10` | `0xF8` | Channel spacing mantissa |
| `DEVIATN` | `0x15` | `0xDF11` | `0x35` | 20.6 kHz frequency deviation |
| `MCSM2` | `0x16` | `0xDF12` | `0x07` | RX timeout configuration |
| `MCSM1` | `0x17` | `0xDF13` | `0x00` | Go to `IDLE` after transmission (`0x30` on RX) |
| `MCSM0` | `0x18` | `0xDF14` | `0x18` | Auto-calibrate PLL on transition `IDLE` $\rightarrow$ `TX`/`RX` |
| `FOCCFG` | `0x19` | `0xDF15` | `0x16` | Frequency Offset Compensation |
| `BSCFG` | `0x1A` | `0xDF16` | `0x6C` | Bit Synchronization |
| `AGCCTRL2` | `0x1B` | `0xDF17` | `0x43` | AGC control |
| `AGCCTRL1` | `0x1C` | `0xDF18` | `0x40` | AGC control |
| `AGCCTRL0` | `0x1D` | `0xDF19` | `0x91` | AGC control |
| `FREND1` | `0x21` | `0xDF21` | `0x56` | Front-end RX configuration |
| `FREND0` | `0x22` | `0xDF22` | `0x10` | Front-end TX (`PA_POWER = 0` $\rightarrow$ reads `PA_TABLE0`) |
| `FSCAL3` | `0x23` | `0xDF23` | `0xE9` | Synthesizer calibration |
| `FSCAL2` | `0x24` | `0xDF24` | `0x2A` | Synthesizer calibration |
| `FSCAL1` | `0x25` | `0xDF25` | `0x00` | Synthesizer calibration |
| `FSCAL0` | `0x26` | `0xDF26` | `0x1F` | Synthesizer calibration (*Note: `0xDF26` in the CC1110 is `PA_TABLE7`! Is `FSCAL0` on the CC1110 located at `0xDF26`? Check against `SWRS033` Table 61!*) |
| `TEST2` | `0x2C` | `0xDF29` | `0x81` | Test register (`0x88` for wider RX) |
| `TEST1` | `0x2D` | `0xDF2A` | `0x35` | Test register (`0x31` for wider RX) |
| `TEST0` | `0x2E` | `0xDF2B` | `0x09` | Test register |
| `PA_TABLE0` | `0x3E` | `0xDF2E` | `0xC2` | `+10 dBm` output power at 868 MHz |