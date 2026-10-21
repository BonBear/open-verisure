# Radioregister & Klockkonfiguration (CC1110 / CC1101)

Både CC1110-sändaren och ESP32-C3-mottagaren (CC1101) använder exakt samma radioparametrar i 868 MHz-bandet (inom SAW-filtrets 868–870 MHz passband).

---

## 1. Systemklocka på CC1110 (26 MHz XOSC)

Radions PLL/frekvenssynthesizer kräver att den externa 26 MHz-kristallen (`X1`) är vald som systemklocka:

```c
void clock_init(void) {
    SLEEP &= ~0x04;          // Slå på båda oscillatorerna (OSC_PD = 0)
    while (!(SLEEP & 0x40)); // Vänta tills 26 MHz XOSC är stabil (XOSC_STB = 1)
    CLKCON &= ~0x47;         // Välj 26 MHz XOSC (OSC = 0) och 26 MHz CLKSPD/TICKSPD
    while (CLKCON & 0x40);   // Bekräfta att klockbytet slagit igenom
    SLEEP |= 0x04;           // Stäng av 13 MHz RC-oscillatorn
}
```

---

## 2. Övergripande Radioparametrar

* **Centerfrekvens:** 868.950 MHz (vid 26.000 MHz kristall)
* **Datahastighet:** 38.383 kBaud ($\approx 38.4\text{ kBaud}$)
* **Modulation:** 2-FSK
* **Deviation:** $\approx 20.6\text{ kHz}$
* **Synkord:** `0xD3`, `0x91` (30/32-bitars synkordsmatchning)
* **Paketformat:** Fast längd (8 bytes payload: `"HB:ALIVE"`) + hårdvaru-CRC påslagen
* **Uteffekt:** `+10 dBm` (`0xC2` på `0xDF2E`)

---

## 3. Komplett Registertabell

| Register | CC1101 SPI-adress | CC1110 XDATA-adress | Värde | Beskrivning |
| :--- | :--- | :--- | :--- | :--- |
| `SYNC1` | `0x04` | `0xDF00` | `0xD3` | Sync word, high byte |
| `SYNC0` | `0x05` | `0xDF01` | `0x91` | Sync word, low byte |
| `PKTLEN` | `0x06` | `0xDF02` | `0x08` | Fast paketlängd = 8 bytes |
| `PKTCTRL1` | `0x07` | `0xDF03` | `0x04` | `APPEND_STATUS = 1` (RSSI + LQI/CRC) |
| `PKTCTRL0` | `0x08` | `0xDF04` | `0x04` | Fixed packet length (`00`), `CRC_EN = 1` (`0x22` = CW-testläge) |
| `CHANNR` | `0x0A` | `0xDF06` | `0x00` | Kanalnummer 0 |
| `FSCTRL1` | `0x0B` | `0xDF07` | `0x06` | IF-frekvens (152.3 kHz) |
| `FSCTRL0` | `0x0C` | `0xDF08` | `0x00` | Frekvensoffset |
| `FREQ2` | `0x0D` | `0xDF09` | `0x21` | Frekvensord high byte (868.950 MHz) |
| `FREQ1` | `0x0E` | `0xDF0A` | `0x6B` | Frekvensord middle byte |
| `FREQ0` | `0x0F` | `0xDF0B` | `0xD1` | Frekvensord low byte |
| `MDMCFG4` | `0x10` | `0xDF0C` | `0x8A` | RX-filter 203 kHz (`0x5A` = 325 kHz), `DRATE_E = 10` |
| `MDMCFG3` | `0x11` | `0xDF0D` | `0x83` | `DRATE_M = 131` (38.383 kBaud) |
| `MDMCFG2` | `0x12` | `0xDF0E` | `0x03` | 2-FSK, 30/32-bitars synkord (`0x00` vid CW-test) |
| `MDMCFG1` | `0x13` | `0xDF0F` | `0x22` | 4 bytes preamble |
| `MDMCFG0` | `0x14` | `0xDF10` | `0xF8` | Kanalavstånd mantissa |
| `DEVIATN` | `0x15` | `0xDF11` | `0x35` | 20.6 kHz frekvensdeviation |
| `MCSM2` | `0x16` | `0xDF12` | `0x07` | RX-timeout-konfiguration |
| `MCSM1` | `0x17` | `0xDF13` | `0x00` | Gå till `IDLE` efter sändning (`0x30` på RX) |
| `MCSM0` | `0x18` | `0xDF14` | `0x18` | Autokalibrera PLL vid övergång `IDLE` $\rightarrow$ `TX`/`RX` |
| `FOCCFG` | `0x19` | `0xDF15` | `0x16` | Frequency Offset Compensation |
| `BSCFG` | `0x1A` | `0xDF16` | `0x6C` | Bit Synchronization |
| `AGCCTRL2` | `0x1B` | `0xDF17` | `0x43` | AGC-kontroll |
| `AGCCTRL1` | `0x1C` | `0xDF18` | `0x40` | AGC-kontroll |
| `AGCCTRL0` | `0x1D` | `0xDF19` | `0x91` | AGC-kontroll |
| `FREND1` | `0x21` | `0xDF21` | `0x56` | Front-end RX-konfiguration |
| `FREND0` | `0x22` | `0xDF22` | `0x10` | Front-end TX (`PA_POWER = 0` $\rightarrow$ läser `PA_TABLE0`) |
| `FSCAL3` | `0x23` | `0xDF23` | `0xE9` | Synthesizer-kalibrering |
| `FSCAL2` | `0x24` | `0xDF24` | `0x2A` | Synthesizer-kalibrering |
| `FSCAL1` | `0x25` | `0xDF25` | `0x00` | Synthesizer-kalibrering |
| `FSCAL0` | `0x26` | `0xDF26` | `0x1F` | Synthesizer-kalibrering (*Obs: `0xDF26` i CC1110 är `PA_TABLE7`! `FSCAL0` på CC1110 ligger på `0xDF26`? Kontrollera mot `SWRS033` Table 61!*) |
| `TEST2` | `0x2C` | `0xDF29` | `0x81` | Testregister (`0x88` för bredare RX) |
| `TEST1` | `0x2D` | `0xDF2A` | `0x35` | Testregister (`0x31` för bredare RX) |
| `TEST0` | `0x2E` | `0xDF2B` | `0x09` | Testregister |
| `PA_TABLE0` | `0x3E` | `0xDF2E` | `0xC2` | `+10 dBm` uteffekt vid 868 MHz |