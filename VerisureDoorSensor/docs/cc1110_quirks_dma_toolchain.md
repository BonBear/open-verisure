# CC1110 Architecture Pitfalls, DMA, SDCC & Flash Automation

This document describes the critical hardware and compiler differences on the TI CC1110F32 that must be handled to keep the transmitter from getting stuck in silence or in `TXFIFO_UNDERFLOW`.

---

## 1. Critical Hardware Differences (CC1110 vs CC1101)

### 1.1 Radio register addresses (`FSCAL0`, `PA_TABLE0`)
The CC1110's XDATA radio registers do not sit at `0xDF00` + the CC1101 SPI address: `FSCAL0` is at `0xDF1F` (not `0xDF26`, which is reserved) and `PA_TABLE0` is at `0xDF2E`. Use the `<cc1110.h>` names as `radio_init()` in `main.c` does. The register table, the 27 MHz crystal math (`FREQ = 0x202EEF`, `MDMCFG3 = 0x75`, ~21.4 kHz deviation, `PKTLEN = 24`) and this trap are documented in one place: [verisure_sensor_hardware.md](../../documentation/verisure_sensor_hardware.md#-rf-hardware-saw-filter-and-crystal-calculations).

### 1.2 The CC1110 has no 64-byte FIFO – `RFD` is 1 byte!
* Unlike the CC1101, the CC1110 has **no** 64-byte TX/RX FIFO and **no** `TXBYTES`/`RXBYTES` registers (`0xDF3A` is `RSSI` and `0xDF3B` is `MARCSTATE`).
* The CC1110 lacks the `SFTX` command strobe (only `SFSTXON=0x00`, `SCAL=0x01`, `SRX=0x02`, `STX=0x03`, `SIDLE=0x04`, `SRES=0x05` exist).
* `RFD` (SFR `0xD9` / XDATA `0xDFD9`) is a 1-byte register. When `STX` (`0x03`) is activated, the radio sets `RFTXRXIF = 1` every time `RFD` is ready for the next byte. If a byte is not written in time, `MARCSTATE` goes to `0x16` (`TXFIFO_UNDERFLOW`).

### 1.3 `RFIF` (SFR `0xE9`) Bitmasks on the CC1110
Always check the bits according to `SWRS033` (section 13.3):
* Bit 7 (`0x80`): `IRQ_TXUNF` — TX FIFO underflow
* Bit 6 (`0x40`): `IRQ_RXOVF` — RX FIFO overflow
* Bit 5 (`0x20`): `IRQ_TIMEOUT` — RX timeout
* Bit 4 (`0x10`): `IRQ_DONE` — Packet sent/received complete!
* Bit 0 (`0x01`): `IRQ_SFD` — Sync word sent/received

### 1.4 DMA Configuration for Radio TX (TI Reference Method)
To guarantee that `RFD` never suffers an underflow, TI's reference code uses DMA Channel 0:
* **Trigger:** `19` (`0x13`, `DMA_TRIG_RADIO`). (Note: According to SWRS033 Table 51, Trigger 18 is `FLASH` and Trigger 19 is `RADIO`!).
* **Source:** Buffer in `__xdata` (`static __xdata uint8_t tx_buf[PACKET_LEN]`, 24 bytes; the descriptor length is 24).
* **Destination:** `0xDFD9` (`X_RFD`).
* **Byte 7 of the descriptor:** `0x4A` (`SRCINC = 01`, `DESTINC = 00`, `IRQMASK = 1`, `M8 = 0`, `PRIORITY = 10` High).
* **Sequence:**
  1. `RFST = 0x04;` (`SIDLE`)
  2. Configure `DMA0CFGH` / `DMA0CFGL` to the descriptor's address in XDATA.
  3. Clear `RFTXRXIF = 0;` and `RFIF = 0;`.
  4. Arm the DMA: `DMAARM = 0x01;` followed by at least 9 `NOP` instructions (`__asm nop __endasm;`).
  5. Start transmission: `RFST = 0x03;` (`STX`).
  6. Wait on `while (!(RFIF & 0x10));` (`IRQ_DONE`).

---

## 2. Toolchain & Automation in Antigravity

### 2.1 SDCC Compilation
`build_and_flash.py` compiles with `--model-small` and these memory limits for the CC1110F32 (32 KB flash, 4 KB XRAM). Code is limited to `--code-size 0x7800` because flash pages 30-31 (`0x7800`-`0x7FFF`) hold the persistent packet counter log (see "Persistent packet counter" in `main.c`); the linker then fails instead of placing code there.
```powershell
cd VerisureDoorSensor
if (!(Test-Path build)) { New-Item -ItemType Directory -Path build }
sdcc -mmcs51 --model-small --code-size 0x7800 --xram-loc 0xF000 --xram-size 0x0E00 -DCTR_SEED=1UL -c main.c -o build/
sdcc -mmcs51 --model-small --code-size 0x7800 --xram-loc 0xF000 --xram-size 0x0E00 build/main.rel -o build/main.ihx
packihx build/main.ihx > build/main.hex
makebin -p build/main.ihx build/main.bin
```
`CTR_SEED` is the first counter block after a reflash. The script sets it from the gateway (`GET <gateway>/api/replay?id=<ID>`) so the counter continues; built by hand with `1UL`, run `replay reset <ID>` on the gateway's serial console after flashing.

### 2.2 Flashing through the ESP32 CC-Flasher (`http://cc.local`)
A separate ESP32 running [ESP_CC_Flasher](https://github.com/atc1441/ESP_CC_Flasher) acts as the CC debugger. `python VerisureDoorSensor/build_and_flash.py` drives its HTTP API:
1. `POST /cc_init`: enter debug mode and read the chip ID (with `--erase`, `POST /erase_cc` then unlocks a protected chip and `/cc_init` runs again).
2. `POST /edit`: upload `build/main.bin` to the flasher's file system.
3. `POST /flash_cc?file=main.bin`: program the CC1110.
4. Poll `GET /get_status` until the log reports the verify result.
5. `POST /cc_exit_dbg`: leave debug mode and run the firmware.

`--serial <port>` flashes over the flasher's USB serial port instead of Wi-Fi; `--compile-only` only builds.
