/*
 * Flash log of the persistent packet counter (format and rules: the "Persistent packet counter"
 * comment in either main.c), shared by the door sensor and the camera. Included by both main.c
 * files; not compiled on its own. The includer defines CTR_PAGE0, CTR_ENTRIES, CTR_BLOCK_MAX,
 * CTR_PAGE_ADDR(), X_FWDATA_H/L, DMA_TRIG_FLASH, WDT_FEED(), dma_desc, ctr_buf, ctr_page and
 * ctr_next (their memory class may differ between the devices), and ctr_boot(), which sets
 * ctr_limit, ctr_safe and ctr_retry below before the first ctr_log().
 *
 * Residual risks (each needs a fault outside the flash specification, and none is detected):
 * an entry that reads back correctly and loses charge later (the next boot then starts at the
 * entry before it + 2, which can be a block already used; a second read-back right after the
 * write reads the same cells at the same moment and would not see it), and an erase that
 * pre-programs cells to 0 before erasing (the CC1110 documents no such step): cut, it could
 * leave a 0-heavy pattern that passes the 0-bit count (e.g. FF 01 FF FE = block 0xFEFFFF). That
 * can only happen in the page that does not hold the newest entry, and a valid entry there can
 * only raise the counter (skip ahead, at worst hold every packet at CTR_BLOCK_MAX), never reuse.
 */

/* The highest block a packet may use: the newest block in the log + 1. The next boot starts at
 * newest + 2, so packets up to here are never reused, even if this block could not be logged. */
static __xdata uint32_t ctr_limit;
/* ctr_page holds the newest entry (or the log has none): only then may the other page be
 * erased. False from a page switch until an entry in the new page reads back. */
static __xdata bool ctr_safe;
/* Early erases of a page without a valid entry since the boot (CTR_RETRY_MAX at most). */
static __xdata uint8_t ctr_retry;
#define CTR_RETRY_MAX 8

static const __code uint8_t *ctr_entry(uint8_t page, uint16_t i) {
    return (const __code uint8_t *)(CTR_PAGE_ADDR(page) + (i << 2));
}

static bool ctr_erased(const __code uint8_t *e) {
    return (e[0] & e[1] & e[2] & e[3]) == 0xFF;
}

/** @brief The check byte of an entry: the number of 0 bits in the 24-bit block (a Berger
 *  code). A cut write or erase and a worn cell only leave bits at 1: that lowers the count of
 *  the block's bytes and can only raise the stored check, so such an entry never passes. */
static uint8_t ctr_check(uint32_t block) {
    uint8_t n = 24, i, b;
    for (i = 0; i < 3; i++) {
        b = (uint8_t)block;
        block >>= 8;
        while (b) {             // one pass per 1 bit
            n--;
            b &= b - 1;
        }
    }
    return n;
}

/** @return true and the block in *block if the entry is valid. */
static bool ctr_decode(const __code uint8_t *e, uint32_t *block) {
    uint32_t v = (uint32_t)e[0] | ((uint32_t)e[2] << 8) | ((uint32_t)e[3] << 16);
    if (e[3] == 0xFF || e[1] != ctr_check(v))
        return false;
    *block = v;
    return true;
}

/** @brief Flash timing (FWT) is set for the 27 MHz crystal: no flash operation without it. */
static bool flash_clock_ok(void) {
    return (CLKCON & 0x40) == 0;   // OSC = 0: crystal
}

static void flash_erase_page(uint8_t page) {
    if (!flash_clock_ok()) return;
    WDT_FEED();
    FWT = 0x23;                  // 21000 * 27 MHz / 16e9 = 35.4 (SWRS033 flash timing)
    FADDRH = page << 1;          // word address of the page
    FADDRL = 0;
    FCTL |= 0x01;                // ERASE
    __asm nop __endasm;
    while (FCTL & 0x80);         // BUSY (~20 ms)
}

/** @brief Write the 4 bytes in ctr_buf to flash (word aligned) with DMA, the way the CC1110
 *  needs it when the code runs from flash. */
static void flash_write4(uint16_t addr) {
    if (!flash_clock_ok()) return;
    WDT_FEED();
    FWT = 0x23;
    FADDRH = (uint8_t)(addr >> 9);   // word address = byte address / 2
    FADDRL = (uint8_t)(addr >> 1);
    dma_desc.src_h = (uint8_t)(((uint16_t)ctr_buf) >> 8);
    dma_desc.src_l = (uint8_t)(((uint16_t)ctr_buf) & 0xFF);
    dma_desc.dst_h = X_FWDATA_H;
    dma_desc.dst_l = X_FWDATA_L;
    dma_desc.len_h = 0x00;
    dma_desc.len_l = 4;
    dma_desc.cfg0  = DMA_TRIG_FLASH; // byte size, single mode, trigger FLASH
    dma_desc.cfg1  = 0x42;           // SRCINC=1, DESTINC=0, no IRQ, PRIORITY=High
    DMA0CFGH = (uint8_t)(((uint16_t)&dma_desc) >> 8);
    DMA0CFGL = (uint8_t)(((uint16_t)&dma_desc) & 0xFF);
    DMAIRQ &= ~0x01;
    DMAARM = 0x01;
    __asm
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
    __endasm;
    FCTL |= 0x02;                    // WRITE: each byte written to FWDATA by the DMA
    while (FCTL & 0x80);             // BUSY until all 4 bytes are written
}

/** @brief Append a block number to the log (up to 3 slots if a write does not read back) and
 *  raise ctr_limit to block + 1.
 *  A page without a valid entry yet (ctr_safe false: the newest entry is in the other page) is
 *  erased again when it is full, or when its next slot is not blank (an erase that did not
 *  finish: weak supply, cells stuck at 0; at most CTR_RETRY_MAX times per boot, then only when
 *  full, so a dead page costs one erase per ~85 calls). The page with the newest entry is never
 *  erased.
 *  @return false if no slot read back (worn page, weak supply), without the crystal (nothing
 *  is written, so no page is switched without its erase), or above CTR_BLOCK_MAX (the counter
 *  never wraps). */
static bool ctr_log(uint32_t block) {
    uint8_t tries;
    uint16_t addr;
    uint32_t got;

    if (block > CTR_BLOCK_MAX || !flash_clock_ok())
        return false;
    ctr_buf[0] = (uint8_t)block;
    ctr_buf[1] = ctr_check(block);
    ctr_buf[2] = (uint8_t)(block >> 8);
    ctr_buf[3] = (uint8_t)(block >> 16);
    for (tries = 0; tries < 3; tries++) {
        if (ctr_next >= CTR_ENTRIES || (!ctr_safe && ctr_retry < CTR_RETRY_MAX &&
                                         !ctr_erased(ctr_entry(ctr_page, ctr_next)))) {
            if (ctr_safe)
                ctr_page = (ctr_page == CTR_PAGE0) ? CTR_PAGE0 + 1 : CTR_PAGE0;
            else if (ctr_next < CTR_ENTRIES)
                ctr_retry++;
            flash_erase_page(ctr_page);
            ctr_next = 0;
            ctr_safe = false;
        }
        addr = CTR_PAGE_ADDR(ctr_page) + (ctr_next << 2);
        ctr_next++;
        if (ctr_erased(ctr_entry(ctr_page, ctr_next - 1)))   // never program a word twice
            flash_write4(addr);
        if (ctr_decode(ctr_entry(ctr_page, ctr_next - 1), &got) && got == block) {
            ctr_limit = block + 1;
            ctr_safe = true;
            return true;
        }
    }
    return false;
}

/** @brief The counter rule before every packet: a block not in the log yet (a new one, or one
 *  whose entry failed) is logged first.
 *  @return false: do not send the packet (its block is above ctr_limit: it could not be logged
 *  and would be used again after a reset). */
static bool ctr_use(uint32_t block) {
    if (block >= ctr_limit)
        ctr_log(block);
    return block <= ctr_limit;
}
