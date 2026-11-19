/**
 * @file cc1110.h
 * @brief Host stand-in for SDCC's <cc1110.h> and the SDCC keywords, for the counter log tests.
 *        C only (GCC's named address spaces): the firmware code is compiled as C with gcc.
 *
 * __code is the GS segment, whose base emu.c sets to emu_flash, so a __code pointer made from
 * a flash address (as ctr_entry() does) reads the emulated flash at that address. The other
 * memory classes are ordinary memory. The SFRs the log code uses are plain variables, except
 * FCTL: each access goes through emu_fctl(), which carries out an ERASE or WRITE command set by
 * the previous access before it returns (the firmware polls BUSY right after setting it).
 */

#pragma once

#include <stdint.h>

#include "emu.h"

#ifdef __cplusplus
#error "stubs/cc1110.h is for the firmware code (C); include emu.h from C++"
#endif

#define __code   __seg_gs
#define __xdata
#define __data
#define __idata
#define __pdata
#define __interrupt(n)
#define __using(n)
/* __asm nop __endasm; (also over several lines) becomes an empty statement */
#define __asm
#define __endasm
#define nop

extern volatile uint8_t FWT;
extern volatile uint8_t FADDRH;
extern volatile uint8_t FADDRL;
extern volatile uint8_t DMA0CFGH;
extern volatile uint8_t DMA0CFGL;
extern volatile uint8_t DMAARM;
extern volatile uint8_t DMAIRQ;
extern volatile uint8_t CLKCON;
extern volatile uint8_t WDCTL;

#define FCTL (*emu_fctl())
