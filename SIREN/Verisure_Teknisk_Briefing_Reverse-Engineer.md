# Teknisk Briefing: Reverse-Engineering & Implementation av Verisure Siren

Detta dokument sammanfattar all bekräftad hårdvaruarkitektur, RF-radiokonfiguration, kryptografisk protokollstruktur och arbetsmetodik som fastställdes under utvecklingen av Verisures dörr-/chocksensor (`GW-MC-SHOCK`). Använd denna information som bas för att analysera och implementera stöd för **Verisure Siren**.

---

## 1. Radiokonfiguration & Fysikaliskt Lager (Sub-1GHz)

Verisure-ekosystemet använder en standardiserad Sub-1GHz RF-arkitektur centrerad kring Texas Instruments transceiver-familj (CC1100 / CC1101 / CC1110):

* **Centerfrekvens:** `868.950 MHz` (Europeiska SRD/ISM-bandet).
* **Modulation:** `2-FSK`.
* **Datahastighet:** `38.383 kBaud` (nominellt ~38.4 kBaud).
* **Frekvensdeviation:** `21.4 kHz` (`DEVIATN = 0x35`).
* **Synkroniseringsord (Sync Word):** `0xD3 0x91` (16-bitars / 30/32 match).
* **Preamble:** 4 bytes (`0xAA`).
* **Hårdvaru-CRC:** 2 bytes CCITT CRC (beräknas automatiskt av CC1101/CC1110 hårdvara).
* **Referenskristall (Kritisk fälla!):**
  - Verisure PCB använder en extern kristall på **`27.000 MHz`** (märkning "27.000"), **inte** standard 26.000 MHz.
  - Formel för syntes av 868.950 MHz med 27.000 MHz kristall:
    $$\text{FREQ} = \frac{868.950 \times 10^6 \times 2^{16}}{27.000 \times 10^6} = 2\,109\,167 = \text{0x202EEF}$$
    `FREQ2 = 0x20`, `FREQ1 = 0x2E`, `FREQ0 = 0xEF`.
  - Modemkonfiguration för 27 MHz: `MDMCFG4 = 0x8A`, `MDMCFG3 = 0x75`.
* **Transceiver Power (PA Table):** `+10 dBm` (`PA_TABLE0 = 0xC2`).
* **FSCAL0-fälla (SDCC-bugg på CC1110):**
  - I SDCC pekar `PA_TABLE0` felaktigt på `0xDF26` som egentligen är `FSCAL0`.
  - `FSCAL0` **måste** alltid förbli `0x1F` (charge pump calibration), annars tappar PLL-synthesizern faslåsning.
  - Sann `PA_TABLE0` ligger på `0xDF2E`.

---

## 2. Kryptografi & Nyckelhantering (KDF)

Protokollet bygger på symmetrisk **AES-128-ECB** med en tvåstegs nyckelhärledning (Key Derivation Function):

1. **Master Key (Gateway / Hemmanätverk):**
   - 16 bytes (128-bitars hex, t.ex. `00112233445566778899AABBCCDDEEFF`).
   - Delas av hela systemet och konfigureras i gatewayens NVS/`secrets.h`.
2. **Device ID:**
   - Varje enhet har ett unikt 8-teckens ASCII-ID tryckt på etikett/streckkod (t.ex. `2JHL2MVN`).
3. **Sensor Device Key Derivation:**
   - Enhetens individuella AES-nyckel beräknas deterministiskt genom att köra AES-128-ECB kryptering över en 16-bytes härledningsvektor:
     - Byte 0..7: Device ID (8 bytes ASCII).
     - Byte 8..15: Speglad / inverterad Device ID (t.ex. med XOR `0x55` eller padding).
   - Den resulterande 16-bytes chiffertexten utgör sensorns `DEVICE_AES_KEY`.
   - Detta gör att gatewayen med en enda Master Key kan dekryptera och autentisera vilken parkopplad enhet som helst genom att läsa av enhetens öppna ID i pakethuvudet.

---

## 3. Paketstruktur (Dörrsensorns 24-bytes format)

Sensorns paket består av 24 bytes fast längd (+ 2 bytes RF status/CRC):

| Byte-offset | Fält | Beskrivning |
| :--- | :--- | :--- |
| `0 .. 7` | **Header (Klartext)** | 8-teckens ASCII Device ID (t.ex. `"2JHL2MVN"`). |
| `8 .. 23` | **Ciphertext (AES-128)** | 16 bytes krypterad payload med sensorns enhetsnyckel. |

### Dekrypterad Payload (16 Bytes):
* **Byte 0..1:** 16-bitars rullande sekvensräknare (Anti-replay protection).
* **Byte 2:** Status-bitmask (`0x01` Reed öppen, `0x02` Vibration/Chock, `0x04` Tamper/Sabotage, `0x08` Power-On Reset, `0x10` Låg batterispänning).
* **Byte 3:** Batteriprocent (`0..100%`).
* **Byte 4..5:** Batterispänning i millivolt (16-bit Big-Endian, t.ex. `3367` mV).
* **Byte 6..9:** Device ID Echo (de första 4 tecknen av Device ID för anti-spoofing).
* **Byte 10..11:** Magiska signaturbytes `"VS"` (`0x56`, `0x53`) för äkthetsvalidering.
* **Byte 12..15:** Hårdvarudiagnostik (GPIO-portstatusar och återställningsorsak).

---

## 4. Analys av Verisure Siren: Vad som ska undersökas

Till skillnad från dörrsensorn som primärt är en **sändare (TX-only med sleep)** är en siren antingen en **mottagare (RX)** eller en **dubbelriktad nod (transceiver)**:

### A. Hårdvaruinventering (PCB & Silicon):
1. **Mikrokontroller / SoC:**
   - Är den baserad på **TI CC1110F32** (SoC med integrerad radio), eller en separat MCU (t.ex. PIC, STM8, MSP430) kopplad till en diskret **TI CC1101** via SPI?
2. **Kristall:**
   - Verifiera kristallens frekvens (`X1`). Är den 27.000 MHz eller standard 26.000 MHz?
3. **Strömförsörjning & Batteri:**
   - Hur matas sirenen? (Flera D/C-celler, 9V, eller extern 12V?).
   - Lågströmsarkitektur: Lyssnar sirenen kontinuerligt på radio (kräver ~15 mA ström), eller använder den **Wake-On-Radio (WOR)** där den sover och bara sniffar på kanalen periodvis (t.ex. var 500:e ms)?
4. **Programmerings-/Debug-header:**
   - Leta efter testpaddar (likt `J2` på dörrsensorn):
     - `VCC`, `GND`, `DC` (Debug Clock), `DD` (Debug Data), `RESET_N`.
   - Notera att fabrikslåsta CC1110-chip kräver hårdvaru-**Chip Erase** för att låsa upp debug-interfacet innan minnet kan läsas eller skrivas.
5. **Sirendrivsteg / Piezo / Blixt:**
   - Identifiera vilka GPIO-pinnar som styr MOSFET/transistor till piezohornet och eventuella blixtlysdioder.
   - Sabotagebrytare (Tamper switch): Identifiera microswitch för väggfäste och lock.

### B. Protokollfrågor för sirenen:
1. **Kommandostruktur (Downlink Gateway $\rightarrow$ Siren):**
   - Sirenens kommando måste innehålla instruktioner för: *Aktivera siren*, *Inaktivera siren*, *Stroboskop/blixt*, *Batteristatusförfrågan* och eventuell *Volym/Ton*.
   - Hur ser pakethuvudet ut? Riktar gatewayen paketet till sirenens specifika `Device ID`?
   - Vilken padding och initialiseringsvektor används för downlink-AES-blocket?
2. **Bekräftelse (Uplink Siren $\rightarrow$ Gateway):**
   - Skickar sirenen ett `ACK`-telemetripaket tillbaka till gatewayen vid triggning?
   - Skickar sirenen ett periodiskt heartbeat (t.ex. 1 gång per timme med batteri-mV och tamperstatus)?

---

## 5. Befintlig Toolchain & Flasher-infrastruktur

Projektet har en komplett, fungerande verktygskedja som kan återanvändas rakt av:
1. **ESP_CC_Flasher (Aaron Christophel / mDNS `http://cc.local`):**
   - ESP32-baserad flasher som ansluter till sensorns debug-header:
     - `3V3` $\rightarrow$ Target VCC
     - `GND` $\rightarrow$ Target GND
     - `GPIO 19` $\rightarrow$ `CC_DC` (Debug Clock)
     - `GPIO 23` $\rightarrow$ `CC_DD` (Debug Data)
     - `GPIO 33` $\rightarrow$ `CC_RST` (Debug Reset)
   - Stödjer REST-anrop för `/cc_init`, `/erase_cc`, `/edit` (upload `.bin`), `/flash_cc`, samt direkt dumpning av flash/XDATA via `/dump_cc` och `/read_xdata`.
2. **ESP32-S2 Gateway (Mottagare):**
   - WEMOS LOLIN S2 Mini kopplad till CC1101 via isolerade ADC1-pinnar (`GPIO 3, 5, 7, 9`).
   - Integrerad med Home Assistant via MQTT Auto-Discovery.
   - Kan modifieras för att sända (TX) sirenkommandon på samma 868.950 MHz-kanal.
3. **Kompilator:**
   - SDCC (Small Device C Compiler) för MCS-51 / 8051 arkitektur.