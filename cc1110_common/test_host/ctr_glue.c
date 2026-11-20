/**
 * @file ctr_glue.c
 * @brief The real counter log code of one device on the emulated CC1110 (see ctr_dev.h).
 *
 * Built once per variant: -DDEV_SYM=<struct name> -DDEV_NAME="<name>", -DDEV_CAM for the
 * camera, plus the variant's macros (CTR_ENTRIES, CTR_SEED). door_ctr.inc / cam_ctr.inc are cut
 * out of the device's main.c by extract_ctr.py and include cc1110_common/ctr_log.c.
 */

#include <cc1110.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ctr_dev.h"
#include "emu.h"

#ifdef DEV_CAM
#include "cam_ctr.inc"
#else
#include "door_ctr.inc"
#endif

static void reset(void) {
    memset((void *)ctr_buf, 0xA5, sizeof(ctr_buf));
    memset((void *)&dma_desc, 0x5A, sizeof(dma_desc));
    ctr_page = 0xA5;
    ctr_next = 0xA5A5;
    ctr_limit = 0xA5A5A5A5UL;
    ctr_safe = true;                 /* the unsafe value: ctr_boot must set it */
    ctr_retry = 0xA5;
#ifdef DEV_CAM
    s_tx_held = true;
#endif
#ifdef DEV_CAM
    s_ctr = 0x5A5A5A5AUL;
#endif
    emu_reset_sfr();
    emu_xdata_clear();
    emu_xdata_register(ctr_buf, sizeof(ctr_buf));
    emu_xdata_register(&dma_desc, sizeof(dma_desc));
}

static uint32_t s_first;
static uint32_t s_arg32;
static uint8_t s_arg8;
static bool s_send;
static uint16_t s_arg16;
static const uint8_t *s_argp;
static bool s_ok;

static void cb_boot(void *a) {
    (void)a;
#ifdef DEV_CAM
    ctr_boot();
    s_first = s_ctr;
#else
    s_first = ctr_boot();
#endif
}

static bool boot(uint32_t *first) {
    bool r = emu_run(cb_boot, 0);
    *first = s_first;
    return r;
}

static void cb_packet(void *a) {
    (void)a;
    s_send = pkt_rule(s_arg32);
}

static bool packet(uint32_t ctr, bool *send) {
    bool r;
    s_arg32 = ctr;
    s_send = false;
    r = emu_run(cb_packet, 0);
    *send = r && s_send;
    return r;
}

static void cb_log(void *a) {
    (void)a;
    s_ok = ctr_log(s_arg32);
}

static bool log_block(uint32_t block, bool *ok) {
    bool r;
    s_arg32 = block;
    s_ok = false;
    r = emu_run(cb_log, 0);
    *ok = s_ok;
    return r;
}

static void cb_erase(void *a) {
    (void)a;
    flash_erase_page(s_arg8);
}

static bool erase_page(uint8_t page) {
    s_arg8 = page;
    return emu_run(cb_erase, 0);
}

static void cb_write4(void *a) {
    (void)a;
    memcpy((void *)ctr_buf, s_argp, 4);
    flash_write4(s_arg16);
}

static bool write4(uint16_t addr, const uint8_t *data) {
    s_arg16 = addr;
    s_argp = data;
    return emu_run(cb_write4, 0);
}

static bool decode(uint8_t page, uint16_t i, uint32_t *block) {
    return ctr_decode(ctr_entry(page, i), block);
}

static bool erased(uint8_t page, uint16_t i) { return ctr_erased(ctr_entry(page, i)); }

static bool clock_ok(void) { return flash_clock_ok(); }

static uint32_t limit(void) { return ctr_limit; }
static unsigned page(void) { return ctr_page; }
static unsigned next(void) { return ctr_next; }
static bool safe(void) { return ctr_safe; }

static void set_log_state(unsigned pg, unsigned nx, uint32_t lim, bool sf) {
    ctr_page = pg;
    ctr_next = nx;
    ctr_limit = lim;
    ctr_safe = sf;
}

#ifdef DEV_CAM
static uint8_t s_cmd;

static void cb_pic_ok(void *a) {
    (void)a;
    s_ok = pic_ctr_ok();
}

static bool pic_ok(uint32_t *ctr, bool *ok) {
    bool r;
    s_ctr = *ctr;
    s_ok = false;
    r = emu_run(cb_pic_ok, 0);
    *ctr = s_ctr;
    *ok = s_ok;
    return r;
}

static void cb_picture(void *a) {
    (void)a;
    s_ok = bf_send_picture(s_cmd, false);
}

static bool picture(uint8_t cmd, uint32_t *ctr, struct pic_trace *trace, bool *ok) {
    bool r;
    memset(&s_pic, 0, sizeof(s_pic));
    s_cmd = cmd;
    s_ctr = *ctr;
    s_ok = false;
    r = emu_run(cb_picture, 0);
    *ctr = s_ctr;
    *trace = s_pic;
    *ok = s_ok;
    return r;
}
#endif

const struct ctr_dev DEV_SYM = {
    DEV_NAME,
#ifdef DEV_CAM
    true,
#else
    false,
#endif
    CTR_ENTRIES, CTR_PAGE0, CTR_SEED, CTR_BLOCK_MAX,
    reset, boot, packet, log_block, erase_page, write4, decode, erased, clock_ok,
    limit, page, next, safe, set_log_state,
#ifdef DEV_CAM
    pic_ok, picture,
#else
    NULL, NULL,
#endif
};
