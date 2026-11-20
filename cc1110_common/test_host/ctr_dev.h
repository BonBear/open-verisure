/**
 * @file ctr_dev.h
 * @brief One build of a device's counter log code (ctr_glue.c), as the tests call it.
 *
 * Each variant is the real code of one main.c (ctr_boot and the counter rule before a packet,
 * cut out by extract_ctr.py) plus the real cc1110_common/ctr_log.c, built with its own macros
 * (e.g. CTR_ENTRIES 4 or another CTR_SEED). The calls that can reach the flash return false
 * when an armed power cut (emu_cut_at) fired inside them.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PIC_TRACE_MAX 64

/** What the camera's picture path sent (pic_stubs.h). */
struct pic_trace {
    unsigned npackets;                     /* 24-byte packets sent by transmit() */
    uint32_t packet_ctr[PIC_TRACE_MAX];
    uint8_t packet_marker[PIC_TRACE_MAX];
    unsigned nchunks;                      /* picture chunks (transmit_big_chunk) */
    uint32_t chunk_tag[PIC_TRACE_MAX];
    unsigned bf_cmds;                      /* picture commands to the Blackfin */
};

struct ctr_dev {
    const char *name;
    bool camera;             /* MD_CAM/firmware/main.c, else VerisureDoorSensor/main.c */
    unsigned entries;        /* CTR_ENTRIES */
    unsigned page0;          /* CTR_PAGE0 */
    uint32_t seed;           /* CTR_SEED as built */
    uint32_t block_max;      /* CTR_BLOCK_MAX */

    /** A reset: RAM (the log's variables, ctr_buf, dma_desc) gets garbage, the SFRs their
     *  reset values, and the DMA sees this variant's XDATA buffers. */
    void (*reset)(void);
    /** ctr_boot(); *first = the first counter value of this boot. */
    bool (*boot)(uint32_t *first);
    /** The firmware's counter rule before the packet with counter @p ctr; *send = the packet
     *  goes out (false: it is held, nothing is sent and the counter stays). */
    bool (*packet)(uint32_t ctr, bool *send);
    /** ctr_log(block) directly; *ok = its result. */
    bool (*log)(uint32_t block, bool *ok);
    /** flash_erase_page() and flash_write4() directly (4 bytes from @p data). */
    bool (*erase_page)(uint8_t page);
    bool (*write4)(uint16_t addr, const uint8_t *data);
    /** ctr_decode() / ctr_erased() of entry @p i of page @p page. */
    bool (*decode)(uint8_t page, uint16_t i, uint32_t *block);
    bool (*erased)(uint8_t page, uint16_t i);
    bool (*clock_ok)(void);

    /** ctr_limit: the highest block a packet may use (the newest logged block + 1). */
    uint32_t (*limit)(void);
    unsigned (*page)(void);
    unsigned (*next)(void);
    bool (*safe)(void);
    void (*set_log_state)(unsigned page, unsigned next, uint32_t limit, bool safe);

    /** Camera only (NULL for the door sensor): the real pic_ctr_ok() and bf_send_picture()
     *  with the stubs of pic_stubs.h, from counter s_ctr = *ctr (updated); *trace = what was
     *  sent (cleared first); *ok = the result. */
    bool (*pic_ctr_ok)(uint32_t *ctr, bool *ok);
    bool (*picture)(uint8_t cmd, uint32_t *ctr, struct pic_trace *trace, bool *ok);
};

extern const struct ctr_dev dev_door, dev_door4, dev_door_seed, dev_door_seed0, dev_door_seedbig;
extern const struct ctr_dev dev_cam, dev_cam_seed;

#ifdef __cplusplus
}
#endif
