/**
 * @file emu.c
 * @brief The emulated CC1110 flash, flash controller and DMA channel 0 (see emu.h).
 */

#include "emu.h"

#include <asm/prctl.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

uint8_t emu_flash[EMU_FLASH_SIZE];
uint8_t emu_stuck1[EMU_FLASH_SIZE];
struct emu_stats emu_stats;
char emu_last_error[256];

volatile uint8_t FWT, FADDRH, FADDRL, DMA0CFGH, DMA0CFGL, DMAARM, DMAIRQ, CLKCON, WDCTL;
static volatile uint8_t s_fctl;


static struct {
    const volatile uint8_t *p;
    unsigned size;
} s_xdata[8];
static unsigned s_nxdata;

static jmp_buf *s_jmp;
static bool s_cut_armed;
static unsigned s_cut_op;
static emu_cut_kind s_cut_kind;
static uint8_t s_keep_lo, s_keep_hi;
static uint32_t s_rng = 1;

static uint32_t rnd(void) {          /* xorshift32 for the partial effects */
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static void error(const char *msg) {
    emu_stats.errors++;
    snprintf(emu_last_error, sizeof(emu_last_error), "%s", msg);
}

void emu_reset_sfr(void) {
    FWT = 0;
    FADDRH = FADDRL = 0;
    DMA0CFGH = DMA0CFGL = 0;
    DMAARM = 0;
    DMAIRQ = 0;
    CLKCON = 0x80;     /* OSC32K = 1 (RC), OSC = 0: the 27 MHz crystal, as after clock_xosc() */
    WDCTL = 0;
    s_fctl = 0;
}

void emu_init(void) {
    static bool gs_set;
    if (!gs_set) {
        if (syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)emu_flash) != 0) {
            perror("arch_prctl(ARCH_SET_GS)");
            abort();
        }
        gs_set = true;
    }
    memset(emu_flash, 0xFF, sizeof(emu_flash));
    memset(emu_stuck1, 0, sizeof(emu_stuck1));
    memset(&emu_stats, 0, sizeof(emu_stats));
    emu_last_error[0] = 0;
    s_cut_armed = false;
    emu_reset_sfr();
}

unsigned emu_state_size(void) {
    return sizeof(emu_flash) + sizeof(emu_stuck1) + sizeof(emu_stats);
}

void emu_save(void *buf) {
    uint8_t *b = (uint8_t *)buf;
    memcpy(b, emu_flash, sizeof(emu_flash));
    b += sizeof(emu_flash);
    memcpy(b, emu_stuck1, sizeof(emu_stuck1));
    b += sizeof(emu_stuck1);
    memcpy(b, &emu_stats, sizeof(emu_stats));
}

void emu_restore(const void *buf) {
    const uint8_t *b = (const uint8_t *)buf;
    memcpy(emu_flash, b, sizeof(emu_flash));
    b += sizeof(emu_flash);
    memcpy(emu_stuck1, b, sizeof(emu_stuck1));
    b += sizeof(emu_stuck1);
    memcpy(&emu_stats, b, sizeof(emu_stats));
}

void emu_xdata_clear(void) { s_nxdata = 0; }

void emu_xdata_register(const volatile void *p, unsigned size) {
    uint16_t a = (uint16_t)(uintptr_t)p;
    for (unsigned i = 0; i < s_nxdata; i++) {
        uint16_t b = (uint16_t)(uintptr_t)s_xdata[i].p;
        if ((uint16_t)(a - b) < s_xdata[i].size || (uint16_t)(b - a) < size) {
            fprintf(stderr, "emu: XDATA objects overlap in their low 16 address bits\n");
            abort();
        }
    }
    if (s_nxdata == sizeof(s_xdata) / sizeof(s_xdata[0]))
        abort();
    s_xdata[s_nxdata].p = (const volatile uint8_t *)p;
    s_xdata[s_nxdata].size = size;
    s_nxdata++;
}

/** Read a byte at an XDATA address (the low 16 bits of a registered object's address). */
static bool xread(uint16_t addr, uint8_t *out) {
    for (unsigned i = 0; i < s_nxdata; i++) {
        uint16_t off = (uint16_t)(addr - (uint16_t)(uintptr_t)s_xdata[i].p);
        if (off < s_xdata[i].size) {
            *out = s_xdata[i].p[off];
            return true;
        }
    }
    return false;
}

void emu_cut_at(unsigned op, emu_cut_kind kind, uint32_t seed) {
    s_cut_armed = true;
    s_cut_op = op;
    s_cut_kind = kind;
    s_rng = seed ? seed : 0x9E3779B9u;
}

void emu_cut_word_at(unsigned op, uint8_t keep_lo, uint8_t keep_hi) {
    emu_cut_at(op, EMU_CUT_PARTIAL, 1);
    s_cut_kind = EMU_CUT_WORD_MASK;
    s_keep_lo = keep_lo;
    s_keep_hi = keep_hi;
}

void emu_cut_clear(void) { s_cut_armed = false; }

bool emu_cut_armed(void) { return s_cut_armed; }

static void power_cut(void) {
    s_cut_armed = false;
    if (!s_jmp) {
        fprintf(stderr, "emu: power cut outside emu_run()\n");
        abort();
    }
    longjmp(*s_jmp, 1);
}

/** Start a flash operation. @return true if the power fails during it (partial effect). */
static bool op_start(void) {
    unsigned n = emu_stats.ops++;
    if (s_cut_armed && n == s_cut_op) {
        if (s_cut_kind == EMU_CUT_BEFORE)
            power_cut();
        return true;   /* the caller makes the partial effect, then cuts */
    }
    return false;
}

/** Random bits, each set with a probability chosen per operation (1/8, 1/2 or 7/8). */
static uint8_t rnd_bits(unsigned density) {
    uint8_t a = (uint8_t)rnd(), b = (uint8_t)rnd(), c = (uint8_t)rnd();
    switch (density) {
    case 0: return a & b & c;
    case 1: return a;
    default: return a | b | c;
    }
}

static void erase_page(unsigned page) {
    uint8_t *p = &emu_flash[page * EMU_PAGE_SIZE];
    emu_stats.last_erase_op = emu_stats.ops;
    if (op_start()) {
        /* A cut erase: cells only move towards 1. Either the page is erased up to some point,
         * or random bits all over it have reached 1, or both. */
        unsigned style = rnd() % 3, density = rnd() % 3;
        unsigned upto = (style != 1) ? rnd() % EMU_PAGE_SIZE : 0;
        for (unsigned i = 0; i < EMU_PAGE_SIZE; i++) {
            if (i < upto)
                p[i] = 0xFF;
            else if (style != 0 || i == upto)
                p[i] |= rnd_bits(density);
        }
        power_cut();
    }
    memset(p, 0xFF, EMU_PAGE_SIZE);
    emu_stats.erases++;
}

static void program_word(uint16_t word, uint8_t lo, uint8_t hi) {
    unsigned a = (unsigned)word * 2;
    uint8_t t0, t1;
    if (a + 1 >= EMU_FLASH_SIZE) {
        error("flash write beyond 32 KB");
        return;
    }
    bool partial = op_start();       /* a cut before the start does not return */
    /* data already there (not 0xFFFF) would be ANDed with the new word: garbage */
    if ((emu_flash[a] & emu_flash[a + 1]) != 0xFF && (lo & hi) != 0xFF)
        emu_stats.rewrites++;
    t0 = lo | emu_stuck1[a];
    t1 = hi | emu_stuck1[a + 1];
    if (partial) {
        /* a cut write: some of the bits to be cleared are still 1 */
        uint8_t k0, k1;
        if (s_cut_kind == EMU_CUT_WORD_MASK) {
            k0 = s_keep_lo;
            k1 = s_keep_hi;
        } else {
            unsigned density = rnd() % 3;
            k0 = rnd_bits(density);
            k1 = rnd_bits(density);
        }
        emu_flash[a] &= (uint8_t)(t0 | (k0 & ~t0));
        emu_flash[a + 1] &= (uint8_t)(t1 | (k1 & ~t1));
        power_cut();
    }
    emu_flash[a] &= t0;
    emu_flash[a + 1] &= t1;
    emu_stats.words++;
}

/** FCTL.WRITE: the controller programs words from FADDR on, two bytes each, as DMA channel 0
 *  (trigger FLASH) delivers them to FWDATA. */
static void flash_write(void) {
    uint8_t d[8];
    uint16_t desc = (uint16_t)((DMA0CFGH << 8) | DMA0CFGL);
    uint16_t word = (uint16_t)(((FADDRH << 8) | FADDRL) & 0x3FFF);
    uint16_t src, dst, len;
    int srcinc, dstinc;
    uint8_t pending = 0, have = 0;

    if (!(DMAARM & 0x01)) {
        error("FCTL.WRITE without DMA channel 0 armed: no data reaches FWDATA");
        return;
    }
    for (unsigned i = 0; i < 8; i++) {
        if (!xread((uint16_t)(desc + i), &d[i])) {
            error("DMA0CFG does not point at a registered DMA descriptor");
            return;
        }
    }
    src = (uint16_t)((d[0] << 8) | d[1]);
    dst = (uint16_t)((d[2] << 8) | d[3]);
    len = (uint16_t)(((d[4] & 0x1F) << 8) | d[5]);
    if ((d[4] >> 5) != 0 || (d[6] & 0x1F) != 18 || (d[6] & 0x80) || ((d[6] >> 5) & 3) != 0) {
        error("DMA descriptor: want fixed length, trigger 18 (FLASH), byte size, single mode");
        return;
    }
    {
        static const int inc[4] = {0, 1, 2, -1};
        srcinc = inc[d[7] >> 6];
        dstinc = inc[(d[7] >> 4) & 3];
    }
    if (len == 0 || (len & 1)) {
        error("DMA length must be a non-zero number of flash words");
        return;
    }
    for (uint16_t i = 0; i < len; i++) {
        uint8_t b;
        uint16_t s = (uint16_t)(src + i * srcinc);
        uint16_t t = (uint16_t)(dst + i * dstinc);
        if (t != EMU_X_FWDATA) {
            error("DMA destination is not FWDATA");
            return;
        }
        if (!xread(s, &b)) {
            error("DMA source is not a registered XDATA object");
            return;
        }
        if (!have) {
            pending = b;
            have = 1;
        } else {
            program_word(word, pending, b);
            word = (uint16_t)((word + 1) & 0x3FFF);
            have = 0;
        }
    }
    FADDRH = (uint8_t)(word >> 8);
    FADDRL = (uint8_t)word;
    DMAARM &= (uint8_t)~0x01;
    DMAIRQ |= 0x01;
}

volatile uint8_t *emu_fctl(void) {
    uint8_t cmd = s_fctl & 0x03;
    if (cmd) {
        s_fctl &= (uint8_t)~0x83;   /* the command runs now; BUSY is clear when it returns */
        if (CLKCON & 0x40)
            error("flash operation while the system clock is not the crystal");
        else if (FWT == 0)
            error("flash operation with FWT not set");
        else if (cmd == 0x03)
            error("FCTL ERASE and WRITE at the same time");
        else if (cmd == 0x01) {
            if (FADDRL != 0 || (FADDRH & 0x01))
                error("page erase: FADDR is not the start of a page");
            erase_page((((FADDRH << 8) | FADDRL) & 0x3FFF) >> 9);
        } else {
            flash_write();
        }
    }
    return &s_fctl;
}

bool emu_run(void (*fn)(void *), void *arg) {
    jmp_buf jb;
    jmp_buf *volatile prev = s_jmp;
    s_jmp = &jb;
    if (setjmp(jb)) {
        s_jmp = prev;
        s_fctl = 0;
        return false;
    }
    fn(arg);
    if (s_fctl & 0x03)
        error("a flash command was set but BUSY was never polled");
    s_jmp = prev;
    return true;
}
