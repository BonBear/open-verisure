/**
 * @file emu.h
 * @brief A small CC1110 emulation for the host tests of the counter log: the 32 KB flash, the
 *        flash controller (FCTL ERASE/WRITE, FADDRH/L, FWT), DMA channel 0 feeding FWDATA, and
 *        power cuts at any point of a flash operation.
 *
 * The flash is NOR: a write only clears bits (new = old & data), an erase sets a 1 KB page to
 * 0xFF. The firmware's __code pointers read it through the x86-64 GS segment (see
 * stubs/cc1110.h), so a code address 0x7800 reads emu_flash[0x7800] as on the chip.
 * A power cut ends the firmware call that runs under emu_run() (longjmp), like a reset; RAM is
 * then re-filled with garbage by the device glue before the next boot.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EMU_FLASH_SIZE 32768u
#define EMU_PAGE_SIZE  1024u
#define EMU_X_FWDATA   0xDFAFu   /* FWDATA in XDATA space */

/** The flash as __code pointers read it. Tests may set bytes directly (e.g. garbage). */
extern uint8_t emu_flash[EMU_FLASH_SIZE];
/** Worn cells: bits set here no longer program (they stay 1 after a write). */
extern uint8_t emu_stuck1[EMU_FLASH_SIZE];
/** Cells stuck at 0: bits set here no longer erase (they stay 0 after an erase). */
extern uint8_t emu_stuck0[EMU_FLASH_SIZE];
/** The next this many page erases fail without a power cut (weak supply): a random part of
 *  the page reaches 1, as in a cut erase, and the firmware goes on. */
extern unsigned emu_erase_fail;

typedef enum {
    EMU_CUT_BEFORE,   /* the power fails as the operation starts: nothing changes */
    EMU_CUT_PARTIAL,  /* the power fails during it: a random part of its effect */
    EMU_CUT_WORD_MASK /* (emu_cut_word_at) a word program cut with chosen bits still 1 */
} emu_cut_kind;

struct emu_stats {
    unsigned ops;        /* flash operations started (page erase or word program) */
    unsigned erases;     /* completed page erases */
    unsigned failed_erases;  /* erases that failed without a power cut (emu_erase_fail) */
    unsigned words;      /* completed word programs */
    unsigned rewrites;   /* words programmed over data (not 0xFFFF): the result is garbage */
    unsigned errors;     /* misuse of the flash controller or the DMA (see emu_last_error) */
    unsigned last_erase_op;  /* operation number of the last page erase started */
};
extern struct emu_stats emu_stats;
extern char emu_last_error[256];

/** CLKCON.OSC (bit 6) = 1: the system clock is the HS RCOSC, flash_clock_ok() is false. */
extern volatile uint8_t CLKCON;

/** A blank chip: all flash 0xFF, no worn or stuck bits, no failing erases, statistics and SFRs
 *  reset, no cut armed. */
void emu_init(void);
/** The whole flash state (contents, worn and stuck bits, failing erases, statistics). */
unsigned emu_state_size(void);
void emu_save(void *buf);
void emu_restore(const void *buf);
/** SFRs after a reset, with the crystal running (both firmwares call clock_xosc() before
 *  they touch the log). */
void emu_reset_sfr(void);
/** XDATA objects the DMA may read (the firmware passes their low 16 address bits). */
void emu_xdata_clear(void);
void emu_xdata_register(const volatile void *p, unsigned size);

/** Cut the power at flash operation number @p op (counted as emu_stats.ops). For
 *  EMU_CUT_PARTIAL, @p seed chooses which part of the operation happened. */
void emu_cut_at(unsigned op, emu_cut_kind kind, uint32_t seed);
/** Cut the power in word program number @p op so that the bits in @p keep_lo / @p keep_hi
 *  that should become 0 are still 1 (the others are programmed). */
void emu_cut_word_at(unsigned op, uint8_t keep_lo, uint8_t keep_hi);
void emu_cut_clear(void);
/** Seed the random partial effects (cut operations, failing erases). */
void emu_seed(uint32_t seed);
bool emu_cut_armed(void);

/** Run firmware code; false if the power was cut inside it. */
bool emu_run(void (*fn)(void *), void *arg);

/** The flash controller as the FCTL macro sees it (stubs/cc1110.h). */
volatile uint8_t *emu_fctl(void);

#ifdef __cplusplus
}
#endif
