# Hårdvarumappning & Pinouts: Verisure CC1110F32 och ESP32-C3

Detta dokument innehåller verifierad pinout och registerkonfiguration för alla sensorer, lysdioder, debug-pinnar och RF-komponenter på Verisure-kortet samt SPI-kopplingen för ESP32-C3.

---

## 1. Verisure Sensorkort (TI CC1110F32) – GPIO & Sensorer

| Sensor / Komponent | CC1110 Pin | Riktning | Register / Bitmask | Funktion & Beteende |
| :--- | :--- | :--- | :--- | :--- |
| **Reed-kontakt** (Magnet) | `P1_7` (Pin 32) | Ingång | `P1SEL &= ~0x80; P1DIR &= ~0x80;` | Känner av om magneten är nära (`0`) eller borta (`1`). |
| **Skaksensor** (Shock/Piezo) | `P1_2` (Pin 1) | Ingång | `P1SEL &= ~0x04; P1DIR &= ~0x04;` | Digital/pulsingång som ger utslag vid vibrationer eller stötar. |
| **Tryckknapp** (`SW1`) | `RESET_N` (Pin 31) | Ingång (Reset) | Hårdvarureset via 100 Ω till GND | Extern hårdvarureset/parningsknapp. Detekteras i mjukvara via `SLEEP.RST[1:0] == 00`. |
| **Röd LED** (`LED_RED`) | `P0_2` | Utgång | `P0SEL &= ~0x04; P0DIR \|= 0x04;` | Aktiv hög (`1` = tänd, `0` = släckt). |
| **Grön LED** (`LED_GREEN`) | `P0_4` | Utgång | `P0SEL &= ~0x10; P0DIR \|= 0x10;` | Aktiv hög (`1` = tänd, `0` = släckt). |
| **Debug Data (`DD`)** | `P2_1` (Pin 21) | I/O | CC-Debugger-gränssnitt | Kopplad till `J2-5` och ESP32 CC-Flasher (`http://cc.local`). |
| **Debug Clock (`DC`)** | `P2_2` (Pin 22) | Ingång | CC-Debugger-gränssnitt | Kopplad till `J2-4` och ESP32 CC-Flasher (`http://cc.local`). |
| **Reset (`RESET_N`)** | `RESET_N` (Pin 31 / Pin 6) | Ingång | Aktiv låg | Kopplad till `J2-6`, ESP32 CC-Flasher och tryckknapp `SW1`. |

---

## 1.1 Verifierad Mappning för Flashing-kontakt (J2)

Fysiska multimetermätningar mot CC1110 och spänningsskenor:

| J2 Pin | Uppmätt Nod | Funktion & Beskrivning |
| :---: | :--- | :--- |
| **`J2-1`** | **Pin 3** | Kvadratisk padd med vit silkscreen-prick. Aux / UART / GPIO. |
| **`J2-2`** | **Pin 2** | Rund padd. Aux / UART / GPIO. |
| **`J2-3`** | **VCC** | **Target 3.3V Power**. Kopplad direkt till batteriets pluspol via säkring `F1` / `BAT+_R`. |
| **`J2-4`** | **Pin 22 (`P2_2`)** | **`DC` (Debug Clock)** för CC-Debugger / ESP-Flasher. |
| **`J2-5`** | **Pin 21 (`P2_1`)** | **`DD` (Debug Data)** för CC-Debugger / ESP-Flasher. |
| **`J2-6`** | **Pin 6** | **Reset / Kontrollpinne** för programmeringsgränssnitt. |
| **`J2-7`** | **GND** | **Systemjord (GND)**. Direkt ansluten till kretskortets jordplan. |

### Pull-up / Pull-down-register
* `P0SEL`, `P1SEL`, `P2SEL`: Sätts till `0` för respektive pinne för att välja vanlig GPIO-funktion.
* `P1INP` / `P2INP`: Konfigurerar pull-up, pull-down eller tristate för portarna.

### Verifierad referenskod för sensorer och lysdioder
```c
#include <cc1110.h>

void setup_gpio(void) {
    // 1. Lysdioder (P0_2 = Röd, P0_4 = Grön) som utgångar
    P0SEL &= ~0x14;
    P0DIR |= 0x14;
    P0_2 = 0;
    P0_4 = 0;

    // 2. Sensorer som ingångar: Reed (P1_7), Skaksensor (P1_2), Knapp (P2_0)
    P1SEL &= ~(0x80 | 0x04);
    P1DIR &= ~(0x80 | 0x04);
    P2SEL &= ~0x01;
    P2DIR &= ~0x01;
}
```

---

## 2. Verisure RF-väg & Kristall (Från Mikroskopanalys)

* **RF-utgångar (`RF_P` / `RF_N`):** Differentiella utgångar (pin 25 och 26 på CC1110) går via ett passivt LC-balun-nätverk rakt in i komponent `U2`.
* **SAW-filter (`U2`, märkt `3716`):** Passivt **EPCOS/TDK B3716** (B39871B3716U410) ytakustiskt bandpassfilter med centerfrekvens **869.00 MHz** och passband **868.00–870.00 MHz**.
* **Kristall (`X1`):** **27.000 MHz** SMD-kristall (graverad "27.000" i mikroskopbilderna `PXL_20261015_200824175.jpg` och `PXL_20261015_200955005~2.jpg`). Kopplad till pin 22/23 på CC1110.
  * **Frekvensregister för 868.950 MHz:** `FREQ2 = 0x20`, `FREQ1 = 0x2E`, `FREQ0 = 0xEF` (teoretiskt fel: endast +27 Hz / 0.03 ppm).
  * **Modemregister för 38.383 kBaud:** `MDMCFG4 = 0x8A`, `MDMCFG3 = 0x75`.
---

## 3. ESP32-C3 Super Mini + CC1101 (Mottagare & Referenssändare)

Både Mottagaren (ESP32 #1) och Referenssändaren (ESP32 #2) använder samma koppling:

| CC1101 Pin | ESP32-C3 Super Mini | Funktion |
| :--- | :--- | :--- |
| **VCC** | `3V3` | Strömförsörjning (3.3 V) |
| **GND** | `GND` | Jord |
| **SCK / SCLK** | `GPIO 4` | SPI Clock |
| **MISO (SO)** | `GPIO 5` | SPI MISO |
| **MOSI (SI)** | `GPIO 6` | SPI MOSI |
| **CSN** | `GPIO 7` | SPI Chip Select |
| **GDO0** | `GPIO 1` | Digital Output / Interrupt |
| **GDO2** | `GPIO 3` | Digital Output / Interrupt |
| *(Inbyggd LED)* | `GPIO 8` | Blå status-LED på Super Mini (Aktiv LÅG) |