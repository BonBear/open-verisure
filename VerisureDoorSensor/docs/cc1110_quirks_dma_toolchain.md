# CC1110 Architecture Pitfalls, DMA, SDCC & Flash Automation

This document describes the critical hardware and compiler differences on the TI CC1110F32 that must be handled to keep the transmitter from getting stuck in silence or in `TXFIFO_UNDERFLOW`.

---

## 1. Critical Hardware Differences (CC1110 vs CC1101)

### 1.1 `PA_TABLE0` and `FSCAL` addresses in SDCC `<cc1110.h>`
According to TI's datasheet (`SWRS033`, Table 61 – Overview of XDATA Radio Registers), the memory map looks like this:
* `0xDF23` = `FSCAL3`
* `0xDF24` = `FSCAL2`
* `0xDF25` = `FSCAL1`
* `0xDF26` = `FSCAL0`
* `0xDF27`..`0xDF2D` = `PA_TABLE7`..`PA_TABLE1`
* **`0xDF2E` = `PA_TABLE0`**

**CRITICAL PITFALL:**
1. In SDCC's `<cc1110.h>`, `PA_TABLE0` is incorrectly set to `0xDF26` (which is in fact **`FSCAL0`**!).
2. If the code writes `X_PA_TABLE_BASE[k] = 0xC2` with base address `0xDF26`, **it overwrites `FSCAL0` (`0xDF26`) with `0xC2`**, which destroys the PLL calibration so that the VCO loses lock!
3. **Correct solution:** Write **only** to `0xDF2E` (`PA_TABLE0`) or `0xDF27..0xDF2E`, and **never touch `0xDF26` (`FSCAL0`) after `FSCAL0 = 0x1F` has been set!**
```c
#define TRUE_FSCAL0      (*((__xdata volatile uint8_t *)0xDF26))
#define TRUE_PA_TABLE0   (*((__xdata volatile uint8_t *)0xDF2E))

TRUE_FSCAL0    = 0x1F; // Must be 0x1F! NEVER write 0xC2 to 0xDF26!
TRUE_PA_TABLE0 = 0xC2; // +10 dBm at 0xDF2E
```

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
* **Source:** Buffer in `__xdata` (e.g. `static __xdata uint8_t tx_buf[8]`).
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

### 2.1 SDCC Compilation (Windows PowerShell)
Compile with `--model-small` and the correct memory limits for the CC1110F32 (32 KB Flash, 4 KB XRAM):
```powershell
if (!(Test-Path build)) { New-Item -ItemType Directory -Path build }
sdcc -mmcs51 --model-small --code-size 0x8000 --xram-loc 0xF000 --xram-size 0x0E00 -c main.c -o build/
sdcc -mmcs51 --model-small --code-size 0x8000 --xram-loc 0xF000 --xram-size 0x0E00 build/main.rel -o build/main.ihx
packihx build/main.ihx > build/main.hex
makebin -p build/main.ihx build/main.bin
```

### 2.2 Automating the ESP32 CC-Flasher (`http://cc.local`)
A separate ESP32 acts as a CC-Debugger at `http://cc.local`.
* **Step 1:** Inspect the web interface by running `curl http://cc.local` from the terminal (or review the CC-Flasher source code if it is available locally) to see which HTTP endpoints handle upload/flashing of `.hex`/`.bin` as well as reset/chip ID.
* **Step 2:** Create a Python or PowerShell script (`flash_cc1110.py`) that uploads `build/main.hex` (or `build/main.bin`) directly to `http://cc.local` and verifies that the chip responds.
* If the web interface on `cc.local` lacks a convenient REST API, we can modify the ESP32 CC-Flasher firmware so that it exposes a direct `POST /flash` endpoint.