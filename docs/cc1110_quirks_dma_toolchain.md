# CC1110 Arkitekturfällor, DMA, SDCC & Flash-automatisering

Detta dokument beskriver de kritiska hårdvaru- och kompilatorskillnaderna på TI CC1110F32 som måste hanteras för att undvika att sändaren fastnar i tystnad eller `TXFIFO_UNDERFLOW`.

---

## 1. Kritiska Hårdvaruskillnader (CC1110 vs CC1101)

### 1.1 `PA_TABLE0` och `FSCAL`-adresser i SDCC `<cc1110.h>`
Enligt TI:s datablad (`SWRS033`, Table 61 – Overview of XDATA Radio Registers) ser minneskartan ut så här:
* `0xDF23` = `FSCAL3`
* `0xDF24` = `FSCAL2`
* `0xDF25` = `FSCAL1`
* `0xDF26` = `FSCAL0`
* `0xDF27`..`0xDF2D` = `PA_TABLE7`..`PA_TABLE1`
* **`0xDF2E` = `PA_TABLE0`**

**KRITISK FÄLLA:**
1. I SDCC:s `<cc1110.h>` är `PA_TABLE0` felaktigt satt till `0xDF26` (vilket i själva verket är **`FSCAL0`**!).
2. Om man i koden skriver `X_PA_TABLE_BASE[k] = 0xC2` med basadress `0xDF26`, **skriver man över `FSCAL0` (`0xDF26`) med `0xC2`**, vilket förstör PLL-kalibreringen så att VCO:n tappar låsning!
3. **Korrekt lösning:** Skriv **endast** till `0xDF2E` (`PA_TABLE0`) eller `0xDF27..0xDF2E`, och **rör aldrig `0xDF26` (`FSCAL0`) efter att `FSCAL0 = 0x1F` har satts!**
```c
#define TRUE_FSCAL0      (*((__xdata volatile uint8_t *)0xDF26))
#define TRUE_PA_TABLE0   (*((__xdata volatile uint8_t *)0xDF2E))

TRUE_FSCAL0    = 0x1F; // Måste vara 0x1F! Skriv ALDRIG 0xC2 till 0xDF26!
TRUE_PA_TABLE0 = 0xC2; // +10 dBm på 0xDF2E
```

### 1.2 CC1110 har inget 64-bytes FIFO – `RFD` är 1 byte!
* Till skillnad från CC1101 har CC1110 **ingen** 64-bytes TX/RX FIFO och **inga** `TXBYTES`/`RXBYTES`-register (`0xDF3A` är `RSSI` och `0xDF3B` är `MARCSTATE`).
* CC1110 saknar kommandostroben `SFTX` (endast `SFSTXON=0x00`, `SCAL=0x01`, `SRX=0x02`, `STX=0x03`, `SIDLE=0x04`, `SRES=0x05` finns).
* `RFD` (SFR `0xD9` / XDATA `0xDFD9`) är ett 1-bytes register. När `STX` (`0x03`) aktiveras sätter radion `RFTXRXIF = 1` varje gång `RFD` är redo för nästa byte. Om en byte inte skrivs i tid går `MARCSTATE` in i `0x16` (`TXFIFO_UNDERFLOW`).

### 1.3 `RFIF` (SFR `0xE9`) Bitmasker på CC1110
Kontrollera alltid bitarna enligt `SWRS033` (avsnitt 13.3):
* Bit 7 (`0x80`): `IRQ_TXUNF` — TX FIFO underflow
* Bit 6 (`0x40`): `IRQ_RXOVF` — RX FIFO overflow
* Bit 5 (`0x20`): `IRQ_TIMEOUT` — RX timeout
* Bit 4 (`0x10`): `IRQ_DONE` — Paket sänt/mottaget klart!
* Bit 0 (`0x01`): `IRQ_SFD` — Synkord (Sync Word) sänt/mottaget

### 1.4 DMA-konfiguration för Radio TX (TI Referensmetod)
För att garantera att `RFD` aldrig råkar ut för underflow använder TI:s referenskod DMA Kanal 0:
* **Trigger:** `19` (`0x13`, `DMA_TRIG_RADIO`). (Obs: Enligt SWRS033 Table 51 är Trigger 18 `FLASH` och Trigger 19 är `RADIO`!).
* **Källa:** Buffert i `__xdata` (t.ex. `static __xdata uint8_t tx_buf[8]`).
* **Mål:** `0xDFD9` (`X_RFD`).
* **Byte 7 i deskriptorn:** `0x4A` (`SRCINC = 01`, `DESTINC = 00`, `IRQMASK = 1`, `M8 = 0`, `PRIORITY = 10` High).
* **Sekvens:**
  1. `RFST = 0x04;` (`SIDLE`)
  2. Konfigurera `DMA0CFGH` / `DMA0CFGL` till deskriptorns adress i XDATA.
  3. Rensa `RFTXRXIF = 0;` och `RFIF = 0;`.
  4. Armera DMA: `DMAARM = 0x01;` följt av minst 9 `NOP`-instruktioner (`__asm nop __endasm;`).
  5. Starta sändning: `RFST = 0x03;` (`STX`).
  6. Vänta på `while (!(RFIF & 0x10));` (`IRQ_DONE`).

---

## 2. Verktygskedja & Automatisering i Antigravity

### 2.1 SDCC Kompilering (Windows PowerShell)
Kompilera med `--model-small` och korrekta minnesgränser för CC1110F32 (32 KB Flash, 4 KB XRAM):
```powershell
if (!(Test-Path build)) { New-Item -ItemType Directory -Path build }
sdcc -mmcs51 --model-small --code-size 0x8000 --xram-loc 0xF000 --xram-size 0x0E00 -c main.c -o build/
sdcc -mmcs51 --model-small --code-size 0x8000 --xram-loc 0xF000 --xram-size 0x0E00 build/main.rel -o build/main.ihx
packihx build/main.ihx > build/main.hex
makebin -p build/main.ihx build/main.bin
```

### 2.2 Automatisera ESP32 CC-Flasher (`http://cc.local`)
En separat ESP32 agerar CC-Debugger på `http://cc.local`.
* **Steg 1:** Inspektera webbgränssnittet genom att köra `curl http://cc.local` från terminalen (eller granska CC-Flasher-källkoden om den finns lokalt) för att se vilka HTTP-endpoints som hanterar uppladdning/flashning av `.hex`/`.bin` samt reset/chip-ID.
* **Steg 2:** Skapa ett Python- eller PowerShell-skript (`flash_cc1110.py`) som laddar upp `build/main.hex` (eller `build/main.bin`) direkt till `http://cc.local` och verifierar att chippet svarar.
* Om webbgränssnittet på `cc.local` saknar ett smidigt REST-API kan vi modifiera ESP32 CC-Flasher-firmwaren så att den exponerar en direkt `POST /flash`-endpoint.