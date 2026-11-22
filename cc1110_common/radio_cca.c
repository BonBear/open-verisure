/*
 * Listen-Before-Talk (CCA) helper for the CC1110 radio core, shared by the door sensor and
 * the camera firmware. Included by both main.c files right before radio_init(); not compiled
 * on its own. The includer provides <cc1110.h>, <stdint.h>, and WDT_FEED().
 */

#ifndef RF_CCA_ENABLE
#define RF_CCA_ENABLE        1
#endif
#ifndef RF_CCA_ABS_THR
#define RF_CCA_ABS_THR       0x04
#endif
#define RF_CCA_MAX_RETRIES   5     /* 1 initial + 5 backoff retries = 6 checks (~20..35 ms backoff) */
#define RF_CCA_SETTLE_H_MS   1     /* 1 x 0.5 ms (~505 us @ 27 MHz) for AGC/RSSI/CS settle */
#define RF_CCA_HALF_MS_ITER  400U  /* down-count iterations for ~0.50 ms @ 27 MHz */

#if RF_CCA_ENABLE
static volatile __xdata uint16_t g_cca_busy = 0;
static volatile __xdata uint16_t g_cca_force = 0;

static void cca_delay_half_ms(uint8_t half_ms) {
    uint16_t i;
    while (half_ms--) {
        WDT_FEED();
        for (i = RF_CCA_HALF_MS_ITER; i != 0; i--) {
            __asm nop __endasm;
        }
    }
}
#endif

/* Preconditions: radio in IDLE (0x01), DMA0 descriptor configured & DMA0CFGH/L loaded,
 * DMA0 NOT armed yet. */
static void radio_cca_start_tx(const __xdata uint8_t *tx_buf_ptr) {
    uint16_t w;
#if RF_CCA_ENABLE
    uint8_t attempt;

    for (attempt = 0; attempt <= RF_CCA_MAX_RETRIES; attempt++) {
        RFTXRXIF = 0;
        RFIF = 0;
        RFST = 0x02; /* SRX from IDLE: FS_AUTOCAL calibrates in ~770 us */
        w = 5000;
        while ((MARCSTATE & 0x1F) != 0x0D && --w);
        if (w) {
            cca_delay_half_ms(RF_CCA_SETTLE_H_MS); /* ~505 us for AGC/RSSI/CS settle */
            if ((MARCSTATE & 0x1F) == 0x0D && (PKTSTATUS & 0x10)) {
                RFTXRXIF = 0;
                RFIF = 0;
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
                RFST = 0x03; /* STX from RX: transitions to TX (0x13) if CCA still clear */
                w = 200;
                while ((MARCSTATE & 0x1F) != 0x13 && --w);
                if (w) {
                    return; /* TX started */
                }
                /* Late CCA rejection: abort DMA0 immediately */
                DMAARM = 0x81;
                __asm nop __endasm;
            }
        }

        if (g_cca_busy < 0xFFFFU) {
            g_cca_busy++;
        }
        RFST = 0x04; /* SIDLE */
        w = 5000;
        while ((MARCSTATE & 0x1F) != 0x01 && --w);
        RFTXRXIF = 0;
        RFIF = 0;
        DMAIRQ &= ~0x01;

        if (attempt < RF_CCA_MAX_RETRIES) {
            uint8_t wait_ms = (uint8_t)(2 + attempt + ((tx_buf_ptr[8 + (attempt & 0x0F)] ^ RSSI) & 0x03));
            cca_delay_half_ms((uint8_t)(wait_ms << 1));
        }
    }
    if (g_cca_force < 0xFFFFU) {
        g_cca_force++;
    }
#else
    (void)tx_buf_ptr;
#endif

    /* Force-TX fallback (also the !RF_CCA_ENABLE path): STX from IDLE bypasses CCA */
    RFST = 0x04; /* SIDLE */
    w = 5000;
    while ((MARCSTATE & 0x1F) != 0x01 && --w);
    RFTXRXIF = 0;
    RFIF = 0;
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
    RFST = 0x03; /* STX */
}
