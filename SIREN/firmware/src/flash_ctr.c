/**
 * @file flash_ctr.c
 * @brief Persistent 32-bit rolling sequence counter in MSP430F149 Info Flash.
 */

#include "flash_ctr.h"
#include <stdbool.h>

#ifndef HOST_TEST
#include <msp430.h>
#include "hal_msp430.h"
#endif

#define CTR_SEG0_ADDR     0x1000U
#define CTR_SEG1_ADDR     0x1080U
#define CTR_SEG_BYTES     128U
#define CTR_SLOTS_PER_SEG 32U
#define CTR_RETRY_MAX     3U

static uint32_t s_cur_block = 1UL;
static uint32_t s_ctr = 0x100UL;
static uint32_t s_ctr_limit = 0x100UL;
static uint8_t  s_ctr_seg = 0U;
static uint8_t  s_ctr_slot = 0U;
static bool     s_ctr_safe = true;
static uint8_t  s_ctr_retry = 0U;

#ifdef HOST_TEST
uint8_t g_mock_info[256];
int g_mock_cut_word1 = 0;
int g_mock_skip_writes = 0;
int g_mock_fail_erase = 0;

void flash_mock_reset(void) {
    uint16_t i;
    for (i = 0; i < (uint16_t)sizeof(g_mock_info); i++) {
        g_mock_info[i] = 0xFFU;
    }
    g_mock_cut_word1 = 0;
    g_mock_skip_writes = 0;
    g_mock_fail_erase = 0;
}
#endif

#ifndef HOST_TEST
static uint16_t seg_base_addr(uint8_t seg) {
    return (seg == 0U) ? CTR_SEG0_ADDR : CTR_SEG1_ADDR;
}
#endif

static void ctr_read_slot(uint8_t seg, uint8_t slot, uint8_t out[4]) {
#ifdef HOST_TEST
    uint16_t idx = (uint16_t)seg * CTR_SEG_BYTES + (uint16_t)slot * 4U;
    out[0] = g_mock_info[idx];
    out[1] = g_mock_info[idx + 1U];
    out[2] = g_mock_info[idx + 2U];
    out[3] = g_mock_info[idx + 3U];
#else
    const volatile uint8_t *p =
        (const volatile uint8_t *)(uintptr_t)(seg_base_addr(seg) + (uint16_t)slot * 4U);
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
    out[3] = p[3];
#endif
}

static bool ctr_erased(const uint8_t e[4]) {
    return (uint8_t)(e[0] & e[1] & e[2] & e[3]) == 0xFFU;
}

static uint8_t ctr_check(uint32_t block) {
    uint8_t n = 24U;
    uint8_t i;
    for (i = 0; i < 3U; i++) {
        uint8_t b = (uint8_t)(block & 0xFFU);
        block >>= 8;
        while (b != 0U) {
            n--;
            b &= (uint8_t)(b - 1U);
        }
    }
    return n;
}

static bool ctr_decode(const uint8_t e[4], uint32_t *block) {
    uint32_t v = (uint32_t)e[0] | ((uint32_t)e[2] << 8) | ((uint32_t)e[3] << 16);
    if (e[3] == 0xFFU || e[1] != ctr_check(v)) {
        return false;
    }
    *block = v;
    return true;
}

#ifdef HOST_TEST
static void flash_write_word(uint8_t seg, uint16_t byte_off, uint16_t w) {
    uint16_t idx = (uint16_t)seg * CTR_SEG_BYTES + byte_off;
    g_mock_info[idx] &= (uint8_t)(w & 0xFFU);
    g_mock_info[idx + 1U] &= (uint8_t)(w >> 8);
}

static void flash_erase_seg(uint8_t seg) {
    uint16_t i;
    uint16_t base;
    if (g_mock_fail_erase > 0) {
        g_mock_fail_erase--;
        return;
    }
    base = (uint16_t)seg * CTR_SEG_BYTES;
    for (i = 0; i < CTR_SEG_BYTES; i++) {
        g_mock_info[base + i] = 0xFFU;
    }
}

static void flash_write_entry(uint8_t seg, uint8_t slot, const uint8_t e[4]) {
    uint16_t off = (uint16_t)slot * 4U;
    uint16_t w0 = (uint16_t)e[0] | ((uint16_t)e[1] << 8);
    uint16_t w1 = (uint16_t)e[2] | ((uint16_t)e[3] << 8);
    if (g_mock_skip_writes > 0) {
        g_mock_skip_writes--;
        return;
    }
    flash_write_word(seg, off, w0);
    if (g_mock_cut_word1 > 0) {
        g_mock_cut_word1--;
        return;
    }
    flash_write_word(seg, off + 2U, w1);
}
#else
static uint16_t msp_save_dint(void) {
    uint16_t sr;
    __asm__ volatile ("mov r2, %0" : "=r" (sr));
    __disable_interrupt();
    __nop();
    return sr;
}

static void msp_restore_int(uint16_t sr) {
    if ((sr & GIE) != 0U) {
        __enable_interrupt();
    }
}

static void flash_wait_not_busy(void) {
    uint16_t to = 60000U;
    while ((FCTL3 & BUSY) != 0U && --to != 0U) {
    }
}

static void flash_erase_seg(uint8_t seg) {
    uint16_t addr = seg_base_addr(seg);
    uint16_t sr = msp_save_dint();
    uint16_t saved_wdt = WDTCTL;

    WDTCTL = WDTPW | WDTHOLD;
    FCTL2 = FWKEY | FSSEL_1 | (hal_xtal_fault() ? 3U : 2U); /* MCLK / 4 on xtal fault, else / 3 */
    flash_wait_not_busy();
    FCTL3 = FWKEY;
    FCTL1 = FWKEY | ERASE;
    *(volatile uint16_t *)(uintptr_t)addr = 0xFFFFU;
    flash_wait_not_busy();
    FCTL1 = FWKEY;
    FCTL3 = FWKEY | LOCK;

    if ((saved_wdt & WDTHOLD) == 0U) {
        WDTCTL = WDTPW | WDTCNTCL | (saved_wdt & 0x00FFU);
    }
    msp_restore_int(sr);
}

static void flash_write_entry(uint8_t seg, uint8_t slot, const uint8_t e[4]) {
    uint16_t addr = seg_base_addr(seg) + (uint16_t)slot * 4U;
    uint16_t w0 = (uint16_t)e[0] | ((uint16_t)e[1] << 8);
    uint16_t w1 = (uint16_t)e[2] | ((uint16_t)e[3] << 8);
    uint16_t sr = msp_save_dint();
    uint16_t saved_wdt = WDTCTL;

    WDTCTL = WDTPW | WDTHOLD;
    FCTL2 = FWKEY | FSSEL_1 | (hal_xtal_fault() ? 3U : 2U); /* MCLK / 4 on xtal fault, else / 3 */
    flash_wait_not_busy();
    FCTL3 = FWKEY;
    FCTL1 = FWKEY | WRT;
    *(volatile uint16_t *)(uintptr_t)addr = w0;
    flash_wait_not_busy();
    *(volatile uint16_t *)(uintptr_t)(addr + 2U) = w1;
    flash_wait_not_busy();
    FCTL1 = FWKEY;
    FCTL3 = FWKEY | LOCK;

    if ((saved_wdt & WDTHOLD) == 0U) {
        WDTCTL = WDTPW | WDTCNTCL | (saved_wdt & 0x00FFU);
    }
    msp_restore_int(sr);
}
#endif

static bool ctr_log(uint32_t block) {
    uint8_t e[4];
    uint8_t tries;

    if (block > CTR_BLOCK_MAX) {
        return false;
    }

    e[0] = (uint8_t)(block & 0xFFU);
    e[1] = ctr_check(block);
    e[2] = (uint8_t)((block >> 8) & 0xFFU);
    e[3] = (uint8_t)((block >> 16) & 0xFFU);

    for (tries = 0; tries < 3U; tries++) {
        uint8_t cur[4];
        uint8_t slot;
        uint32_t got = 0UL;
        bool need_erase = false;

        if (s_ctr_slot >= CTR_SLOTS_PER_SEG) {
            need_erase = true;
        } else if (!s_ctr_safe && s_ctr_retry < CTR_RETRY_MAX) {
            ctr_read_slot(s_ctr_seg, s_ctr_slot, cur);
            if (!ctr_erased(cur)) {
                need_erase = true;
                s_ctr_retry++;
            }
        }

        if (need_erase) {
            if (s_ctr_safe) {
                s_ctr_seg ^= 1U;
            }
            flash_erase_seg(s_ctr_seg);
            s_ctr_slot = 0U;
            s_ctr_safe = false;
        }

        slot = s_ctr_slot;
        s_ctr_slot++;
        ctr_read_slot(s_ctr_seg, slot, cur);
        if (ctr_erased(cur)) {
            flash_write_entry(s_ctr_seg, slot, e);
        }
        ctr_read_slot(s_ctr_seg, slot, cur);
        if (ctr_decode(cur, &got) && got == block) {
            s_cur_block = block;
            s_ctr_limit = (block + 1UL) << 8;
            s_ctr_safe = true;
            return true;
        }
    }

    return false;
}

void ctr_boot(uint32_t seed_block) {
    uint32_t best = 0UL;
    bool found = false;
    uint8_t seg;
    uint8_t used[2] = {0U, 0U};
    uint32_t target;

    for (seg = 0; seg < 2U; seg++) {
        uint8_t i;
        for (i = 0; i < CTR_SLOTS_PER_SEG; i++) {
            uint8_t e[4];
            uint32_t v = 0UL;
            ctr_read_slot(seg, i, e);
            if (ctr_erased(e)) {
                continue;
            }
            used[seg] = (uint8_t)(i + 1U);
            if (ctr_decode(e, &v) && (!found || v > best)) {
                best = v;
                found = true;
                s_ctr_seg = seg;
            }
        }
    }

    if (!found) {
        s_ctr_seg = 0U;
    }
    s_ctr_slot = used[s_ctr_seg];
    s_ctr_safe = true;
    s_ctr_retry = 0U;

    if (found) {
        target = best + 2UL;
    } else {
        target = (seed_block >= 1UL) ? seed_block : 1UL;
    }

    s_cur_block = target;
    s_ctr = target << 8;
    s_ctr_limit = target << 8;

    if (target <= CTR_BLOCK_MAX) {
        (void)ctr_log(target);
    }
}

int ctr_next(uint16_t batt_mv, uint32_t *out_ctr) {
    if (out_ctr == (uint32_t *)0) {
        return -1;
    }

    if (s_ctr >= s_ctr_limit) {
        uint32_t next_block = s_ctr >> 8;
        if (batt_mv < CTR_MIN_BATT_MV) {
            return -1;
        }
        if (s_cur_block >= CTR_BLOCK_MAX || next_block > CTR_BLOCK_MAX) {
            return -1;
        }
        if (!ctr_log(next_block)) {
            return -1;
        }
    }

    *out_ctr = s_ctr;
    s_ctr++;
    return 0;
}

