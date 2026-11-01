# Verisure GW MD CAM2: Hårdvarukartläggning & Mätprotokoll

Detta dokument innehåller den verifierade hårdvarumappningen och mätresultaten för Verisures rörelsedetektor med kamera (**GW MD CAM2**, PCB: `SECUR-PCAM-BLACKFIN P/N3204965 D`, LED-kort: `LED BOARD P/N3302042 C`).

---

## 🎉 Sammanfattning av Dina Mätningar (Fullträff!)

Dina mätningar gav **100 % träff** och stämmer exakt överens både med mikroskopbilden på `U15` (`images/Photo_2026_10_22_00_00_49.jpeg`) och TI:s officiella datablad (`SWRS033`) för CC1110F32:

1. **Programmeringsheadern är `J6`!**
   * `J6-2` = **Pin 15 (`P2_1` / `DD` - Debug Data)**
   * `J6-3` = **Pin 16 (`P2_2` / `DC` - Debug Clock)**
   * `J6-4` = **`RESET_N`** (går även till reset-knappen `SW1` mot `GND`, precis som på dörrsensorn!)
   * `J6-5` = **`GND`**
   * `J6-1` = Har `1 kΩ` mot Pin 10 (`DVDD`). *(Viktigt: Vid flashning matar vi därför 3.3V direkt på `VBAT`-padden eller batteriplus istället för `J6-1` så att inte 1 kΩ-motståndet stryper strömmen!)*
2. **PIR-rörelsesensorn (`ALARM`) är helt kartlagd:**
   * Går direkt till **CC1110 Pin 32 (`P1_7`)** – exakt samma pinne som Verisure använde för magnetkontakten på dörrsensorn!
   * **Aktiv LÅG:** Ligger på **`3.08 V`** i vila och dyker till **`0 V`** vid detekterad rörelse.
3. **Sabotagebrytaren på baksidan (`SW3`):**
   * Ena sidan går till **`VBAT+`** och andra sidan går direkt till **CC1110 Pin 33 (`P1_6`)**! *(Tidigare felaktigt antagen som P0_6)*.
   * **Polaritet:** 
     * **Kåpa av (Tamper Alarm):** Knappen öppen -> Intern pull-down drar `P1_6` till **`0 V`**.
     * **Monterad på vägg (Säkrad):** Knappen sluten mot `VBAT+` -> `P1_6` blir **`3.0 V`** (`1`).
4. **Varför `3.3V`, `2.5V`, `1.8V` och `1.2V` var `0 V` vid batterimätningen:**
   * De 3 batterierna (3V CR123A/litium) ligger **parallellt (`VBAT = 3.12 V`)** och matar **endast CC1110 och PIR-sensorn** i viloläge!
   * Alla spänningsregulatorer för kameran, Blackfin DSP, SDRAM och blixt-LED-kortet (`3.3V`, `2.5V`, `1.8V`, `1.2V`) är **helt avstängda (0 V)** tills CC1110 aktivt väljer att slå på dem! Att de inte slogs på när du viftade beror på att Verisures originalfirmware i CC1110 är "avlarmad" och ignorerar PIR-larm tills larmcentralen begär bild.
5. **SPI- och UART-bussarna mellan CC1110 och Blackfin DSP är helt identifierade:**
   * **SPI (`USART0 Alt. 2`):** `SCK` = Pin 36 (`P1_3`), `MISO` = Pin 35 (`P1_4`), `MOSI` = Pin 34 (`P1_5`), `SS` (övre) = Pin 1 (`P1_2`), `SS` (undre) = Pin 3 (`P1_1`).
   * **UART (`USART1 Alt. 1`):** `TX` = Pin 9 (`P0_4`), `RX` = Pin 11 (`P0_5`).

---

## 📍 Korrekt Pin-guide: TI CC1110F32 (`U15`, QFN-36)

När du läser texten (*"CC1110 F32 TI..."*) rättvänd sitter **Pin 1 uppe till vänster**:

```
             P1_3  P1_4  P1_5  P1_6  P1_7  RST   RBIAS AVDD  AVDD
             (36)  (35)  (34)  (33)  (32)  (31)  (30)  (29)  (28)
           +------------------------------------------------------+
  (1) P1_2 | •                                                    | (27) AVDD
  (2) DVDD |                                                      | (26) AVDD
  (3) P1_1 |                                                      | (25) AVDD
  (4) P1_0 |                                                      | (24) RF_N
  (5) P0_0 |                      CC1110F32                       | (23) RF_P
  (6) P0_1 |                        (U15)                         | (22) AVDD
  (7) P0_2 |                                                      | (21) XOSC_Q1 (27MHz)
  (8) P0_3 |                                                      | (20) XOSC_Q2 (27MHz)
  (9) P0_4 |                                                      | (19) DCOUPL/AGND
           +------------------------------------------------------+
             (10)  (11)  (12)  (13)  (14)  (15)  (16)  (17)  (18)
             DVDD  P0_5  P0_6  P0_7  P2_0  P2_1  P2_2  P2_3  P2_4
                   (RX)  (SW3)             (DD)  (DC)  (32.768kHz)
```

---

## 📋 Verifierad Pinout-tabell för CC1110 (`U15`)

| CC1110 Pin # | Port / Namn | Uppmätt Anslutning på Kretskortet | Funktion |
| :---: | :---: | :--- | :--- |
| **Pin 1** | **`P1_2`** | Testpad **`SS` (Övre)** | SPI Slave Select 1 (`/SPISS`) till Blackfin DSP (hålls HÖG under boot för att undvika Mode Fault) |
| **Pin 2** | `DVDD` | Kondensator `C77` | Matningsavkoppling |
| **Pin 3** | **`P1_1`** | Testpad **`SS` (Undre)** | **Bus Switch Enable** (Aktiv HÖG; hålls HÖG för att öppna SPI-bussen mot DSP/Flash) |
| **Pin 4** | **`P1_0`** | Transistor **`Q7`** | **Power-Enable för 3.3V switched rail** (Kamera/SDRAM) |
| **Pin 5** | **`P0_0`** | Blackfin Reset-krets | **Blackfin Hardware Reset (`/RESET`)** (0=Reset 20mA, 1=Kör 92mA) |
| **Pin 6** | `P0_1` | *(Ej ansluten)* | Oanvänd |
| **Pin 7** | **`P0_2`** | Transistor **`Q13`** | **Power-Enable för 1.2V Core rail** (Blackfin VDDINT) |
| **Pin 8** | `P0_3` | *(Ej ansluten)* | Oanvänd |
| **Pin 9** | **`P0_4`** | Testpad **`TX`** | UART TX (`USART1 Alt. 1`) till Blackfin DSP |
| **Pin 10** | `DVDD` | Kondensator `C91` (+ 1 kΩ mot `J6-1`) | Digital 3V-matning |
| **Pin 11** | **`P0_5`** | Testpad **`RX`** | UART RX (`USART1 Alt. 1`) från Blackfin DSP |
| **Pin 12** | **`P0_6`** | *(Ej ansluten / Oanvänd)* | Oanvänd |
| **Pin 13** | **`P0_7`** | Kontakt **`J2 Pin 4`** / DSP rail | **Power-Enable / Switched Auxiliary rail** (6 kΩ till J2-4) |
| **Pin 14** | `P2_0` | *(Ej ansluten)* | Oanvänd |
| **Pin 15** | **`P2_1` (`DD`)** | **`J6-2`** | **Debug Data (`DD`)** för CC-Flasher |
| **Pin 16** | **`P2_2` (`DC`)** | **`J6-3`** | **Debug Clock (`DC`)** för CC-Flasher |
| **Pin 17** | `P2_3` (`XOSC32K_Q1`) | Kristall `Y2` (32.768 kHz) | Sleep Timer-kristall |
| **Pin 18** | `P2_4` (`XOSC32K_Q2`) | Kristall `Y2` (32.768 kHz) | Sleep Timer-kristall |
| **Pin 19–20** | `XOSC_Q1/Q2` | Kristall `Y1` (**27.000 MHz**) | RF- och systemkristall |
| **Pin 22–23** | `RF_P` / `RF_N` | Balun $\rightarrow$ Fjäderantenn | 868.950 MHz Sub-1GHz RF |
| **Pin 31** | **`RESET_N`** | **`J6-4`** & **`SW1`** | **Hardware Reset** för CC-Flasher |
| **Pin 32** | **`P1_7`** | Testpad **`ALARM`** (från PIR) | **PIR Rörelselarm** (`3.08V` = Vila, `0V` = Rörelse) |
| **Pin 33** | **`P1_6`** | **`SW3`** (Fjäderbrytare baksida) | **Sabotagebrytare (Tamper)** – sluter mot `VBAT+` (0V = Tamper, 3V = Säkrad) |
| **Pin 34** | **`P1_5`** | Testpad **`MOSI`** | Delad SPI MOSI (CC1110 -> Blackfin & U3 Pin 5) |
| **Pin 35** | **`P1_4`** | Testpad **`MISO`** | Delad SPI MISO (Blackfin & U3 Pin 2 -> CC1110) |
| **Pin 36** | **`P1_3`** | Testpad **`SCK`** | Delad SPI SCK (CC1110 -> Blackfin & U3 Pin 6) |

---

## 💡 LED-kortet (`LED BOARD P/N3302042 C`)

Från dina nya bilder (`images/led_board_front.jpg` och `images/led_board_back.jpg`) som ansluts i 4-poliga kontakterna `J3` och `J2`:
* **4 st Högeffekts-LEDs (Blixt/IR):** Lyser upp rummet vid fotografering i mörker.
* **Ljussensor (uppe på fliken ovanför kameralinsen):** Mäter om rummet är mörkt eller ljust.
* **Drivkrets på baksidan (`U2`):** `MCP6042I` (dubbel rail-to-rail operationsförstärkare) + transistorerna `Q3`/`Q6` och `1R0` (1.0 Ω) strömsensor-motstånd som utgör en konstantströmsdrivare för blixt-lysdioderna.

---

## ✅ Kontrollmätningar inför Flashning (Bekräftade!)

1. **Bekräfta strömmatning (`VBAT` mot CC1110):**
   * Piper testpadden **`VBAT`** direkt (0 Ω) mot **CC1110 Pin 10 (`DVDD`)**?
   * **Svar:** **[X] Ja (Piper direkt!)** $\rightarrow$ Vi ansluter `3V3` från `ESP_CC_Flasher` till `VBAT`-padden (med batterierna urtagna). *(1 kΩ-motståndet på `J6-1` är bara en `V_SENSE`-skyddsutgång för fabrikens testjigg).*

2. **Bekräfta `RESET_N` (`J6-4` mot CC1110 Pin 31):**
   * Mät resistans (Ω) mellan **`J6-4`** och **CC1110 Pin 31** (`RESET_N`).
   * **Svar:** **`2.7 kΩ`** $\rightarrow$ Det sitter ett `2.7 kΩ` ESD-skyddsmotstånd i serie mellan `J6-4`/`SW1` och Pin 31 (`RESET_N`). Eftersom CC1110 har en inre pull-up på `20 kΩ` ger `2.7 kΩ` en låg nivå på ~0.37 V och fungerar rakt av för debug-reset (skulle flanken mot förmodan bli för långsam finns Pin 31-vian/motståndet direkt ovanför `U15`).

---

## 🔌 Inkoppling av ESP_CC_Flasher (När ovanstående 2 punkter är bekräftade)

| ESP_CC_Flasher Pin | Kopplas till på MD_CAM | Beskrivning |
| :--- | :--- | :--- |
| **`3V3`** | Testpad **`VBAT`** *(med batterier urtagna!)* | Direkt 3.3V-matning till CC1110 (slipper 1 kΩ på `J6-1`) |
| **`GND`** | **`J6-5`** *(eller testpad `GND`)* | Systemjord |
| **`CC_DD` (GPIO 23)** | **`J6-2`** | Debug Data (`P2_1` / Pin 15) |
| **`CC_DC` (GPIO 19)** | **`J6-3`** | Debug Clock (`P2_2` / Pin 16) |
| **`CC_RST` (GPIO 33)** | **`J6-4`** | Hardware Reset (`RESET_N` / Pin 31) |
