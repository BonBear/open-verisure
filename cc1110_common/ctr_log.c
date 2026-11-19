/*
 * Flash log of the persistent packet counter (format and rules: the "Persistent packet counter"
 * comment in either main.c), shared by the door sensor and the camera. Included by both main.c
 * files; not compiled on its own. The includer defines CTR_PAGE0, CTR_ENTRIES, CTR_BLOCK_MAX,
 * CTR_PAGE_ADDR(), X_FWDATA_H/L, DMA_TRIG_FLASH, WDT_FEED(), dma_desc, ctr_buf, ctr_page and
 * ctr_next (their memory class may differ between the devices), and ctr_boot().
 */

static const __code uint8_t *ctr_entry(uint8_t page, uint16_t i) {
    return (const __code uint8_t *)(CTR_PAGE_ADDR(page) + (i << 2));
}

static bool ctr_erased(const __code uint8_t *e) {
    return (e[0] & e[1] & e[2] & e[3]) == 0xFF;
}

/** @return true and the block in *block if the entry is valid. */
static bool ctr_decode(const __code uint8_t *e, uint32_t *block) {
    if (e[3] == 0xFF || e[1] != (uint8_t)(e[0] ^ e[2] ^ e[3] ^ 0xA5))
        return false;
    *block = (uint32_t)e[0] | ((uint32_t)e[2] << 8) | ((uint32_t)e[3] << 16);
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

/** @brief Append a block number to the log (up to 3 slots if a write does not read back).
 *  @return false if no slot read back (worn page, weak supply, no crystal): see ctr_pending. */
static bool ctr_log(uint32_t block) {
    uint8_t tries;
    uint32_t got;

    if (block > CTR_BLOCK_MAX) block = CTR_BLOCK_MAX;
    ctr_buf[0] = (uint8_t)block;
    ctr_buf[2] = (uint8_t)(block >> 8);
    ctr_buf[3] = (uint8_t)(block >> 16);
    ctr_buf[1] = ctr_buf[0] ^ ctr_buf[2] ^ ctr_buf[3] ^ 0xA5;
    for (tries = 0; tries < 3; tries++) {
        if (ctr_next >= CTR_ENTRIES) {
            ctr_page = (ctr_page == CTR_PAGE0) ? CTR_PAGE0 + 1 : CTR_PAGE0;
            flash_erase_page(ctr_page);
            ctr_next = 0;
        }
        flash_write4(CTR_PAGE_ADDR(ctr_page) + (ctr_next << 2));
        ctr_next++;
        if (ctr_decode(ctr_entry(ctr_page, ctr_next - 1), &got) && got == block)
            return true;
    }
    return false;
}
