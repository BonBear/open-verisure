/**
 * @file cc1101.c
 * @brief TI CC1101 driver for 26.000 MHz crystal at 868.950 MHz (OpenVerisure).
 */

#include "cc1101.h"
#include "hal_msp430.h"

/* CC1101 Configuration Registers */
#define CC_IOCFG2    0x00U
#define CC_IOCFG1    0x01U
#define CC_IOCFG0    0x02U
#define CC_SYNC1     0x04U
#define CC_SYNC0     0x05U
#define CC_PKTLEN    0x06U
#define CC_PKTCTRL1  0x07U
#define CC_PKTCTRL0  0x08U
#define CC_CHANNR    0x0AU
#define CC_FSCTRL1   0x0BU
#define CC_FSCTRL0   0x0CU
#define CC_FREQ2     0x0DU
#define CC_FREQ1     0x0EU
#define CC_FREQ0     0x0FU
#define CC_MDMCFG4   0x10U
#define CC_MDMCFG3   0x11U
#define CC_MDMCFG2   0x12U
#define CC_MDMCFG1   0x13U
#define CC_MDMCFG0   0x14U
#define CC_DEVIATN   0x15U
#define CC_MCSM2     0x16U
#define CC_MCSM1     0x17U
#define CC_MCSM0     0x18U
#define CC_FOCCFG    0x19U
#define CC_BSCFG     0x1AU
#define CC_AGCCTRL2  0x1BU
#define CC_AGCCTRL1  0x1CU
#define CC_AGCCTRL0  0x1DU
#define CC_FREND1    0x21U
#define CC_FREND0    0x22U
#define CC_FSCAL3    0x23U
#define CC_FSCAL2    0x24U
#define CC_FSCAL1    0x25U
#define CC_FSCAL0    0x26U
#define CC_TEST2     0x2CU
#define CC_TEST1     0x2DU
#define CC_TEST0     0x2EU

#define CC_PATABLE   0x3EU
#define CC_TXFIFO    0x3FU
#define CC_RXFIFO    0x3FU

/* Command Strobes */
#define CC_SRES      0x30U
#define CC_SRX       0x34U
#define CC_STX       0x35U
#define CC_SIDLE     0x36U
#define CC_SPWD      0x39U
#define CC_SFRX      0x3AU
#define CC_SFTX      0x3BU

/* Status Registers (read with 0xC0 burst bit) */
#define CC_PARTNUM   0x30U
#define CC_VERSION   0x31U
#define CC_MARCSTATE 0x35U
#define CC_RXBYTES   0x3BU

#define CC_FRAME_LEN 24U

typedef struct {
    uint8_t reg;
    uint8_t val;
} cc_reg_cfg_t;

static const cc_reg_cfg_t s_cc_cfg[] = {
    { CC_IOCFG2,   0x2FU },
    { CC_IOCFG1,   0x2FU },
    { CC_IOCFG0,   0x06U },
    { CC_SYNC1,    0xD3U },
    { CC_SYNC0,    0x91U },
    { CC_PKTLEN,   0x18U },
    { CC_PKTCTRL1, 0x04U },
    { CC_PKTCTRL0, 0x04U },
    { CC_CHANNR,   0x00U },
    { CC_FSCTRL1,  0x06U },
    { CC_FSCTRL0,  0x00U },
    { CC_FREQ2,    0x21U },
    { CC_FREQ1,    0x6BU },
    { CC_FREQ0,    0xD1U },
    { CC_MDMCFG4,  0x8AU },
    { CC_MDMCFG3,  0x83U },
    { CC_MDMCFG2,  0x03U },
    { CC_MDMCFG1,  0x22U },
    { CC_MDMCFG0,  0xF8U },
    { CC_DEVIATN,  0x35U },
    { CC_MCSM2,    0x07U },
    { CC_MCSM1,    0x30U },
    { CC_MCSM0,    0x18U },
    { CC_FOCCFG,   0x16U },
    { CC_BSCFG,    0x6CU },
    { CC_AGCCTRL2, 0x43U },
    { CC_AGCCTRL1, 0x40U },
    { CC_AGCCTRL0, 0x91U },
    { CC_FREND1,   0x56U },
    { CC_FREND0,   0x10U },
    { CC_FSCAL3,   0xE9U },
    { CC_FSCAL2,   0x2AU },
    { CC_FSCAL1,   0x00U },
    { CC_FSCAL0,   0x1FU },
    { CC_TEST2,    0x81U },
    { CC_TEST1,    0x35U },
    { CC_TEST0,    0x09U }
};

static int cc_strobe(uint8_t cmd) {
    if (hal_spi_cs_low() != 0) {
        return -1;
    }
    (void)hal_spi_xfer(cmd);
    hal_spi_cs_high();
    return 0;
}

static int cc_write_reg(uint8_t reg, uint8_t val) {
    if (hal_spi_cs_low() != 0) {
        return -1;
    }
    (void)hal_spi_xfer(reg);
    (void)hal_spi_xfer(val);
    hal_spi_cs_high();
    return 0;
}

static int cc_read_status(uint8_t reg, uint8_t *out) {
    if (hal_spi_cs_low() != 0) {
        return -1;
    }
    (void)hal_spi_xfer((uint8_t)(reg | 0xC0U));
    *out = hal_spi_xfer(0x00U);
    hal_spi_cs_high();
    return 0;
}

static int cc_read_status_safe(uint8_t reg, uint8_t *out) {
    uint8_t v1 = 0U;
    uint8_t n;
    if (cc_read_status(reg, &v1) != 0) {
        return -1;
    }
    for (n = 0U; n < 4U; n++) {
        uint8_t v2 = 0U;
        if (cc_read_status(reg, &v2) != 0) {
            return -1;
        }
        if (v1 == v2) {
            *out = v2;
            return 0;
        }
        v1 = v2;
    }
    *out = v1;
    return 0;
}

static int cc_write_burst(uint8_t addr, const uint8_t *buf, uint8_t len) {
    uint8_t i;
    if (hal_spi_cs_low() != 0) {
        return -1;
    }
    (void)hal_spi_xfer((uint8_t)(addr | 0x40U));
    for (i = 0U; i < len; i++) {
        (void)hal_spi_xfer(buf[i]);
    }
    hal_spi_cs_high();
    return 0;
}

static int cc_read_burst(uint8_t addr, uint8_t *buf, uint8_t len) {
    uint8_t i;
    if (hal_spi_cs_low() != 0) {
        return -1;
    }
    (void)hal_spi_xfer((uint8_t)(addr | 0xC0U));
    for (i = 0U; i < len; i++) {
        buf[i] = hal_spi_xfer(0x00U);
    }
    hal_spi_cs_high();
    return 0;
}

static int cc_enter_idle(void) {
    uint16_t to = 200U;
    uint8_t marc = 0U;
    if (cc_strobe(CC_SIDLE) != 0) {
        return -1;
    }
    while (to != 0U) {
        if (cc_read_status_safe(CC_MARCSTATE, &marc) != 0) {
            return -1;
        }
        if ((marc & 0x1FU) == 0x01U) {
            return 0;
        }
        hal_delay_us(50U);
        hal_wdt_feed();
        to--;
    }
    return -1;
}

int cc1101_init(void) {
    uint16_t to;
    uint8_t i;
    uint8_t part = 0U;
    uint8_t ver = 0U;

    /* Manual power-on reset pulse on CSn per SWRS061I. */
    hal_spi_cs_high();
    hal_delay_us(40U);
    if (hal_spi_cs_low() != 0) {
        return -1;
    }
    hal_delay_us(40U);
    hal_spi_cs_high();
    hal_delay_us(50U);

    if (hal_spi_cs_low() != 0) {
        return -1;
    }
    (void)hal_spi_xfer(CC_SRES);
    to = 2000U;
    while (hal_spi_so_read() != 0U && --to != 0U) {
        if ((to & 0x3FU) == 0U) {
            hal_wdt_feed();
        }
        hal_delay_us(10U);
    }
    hal_spi_cs_high();
    if (to == 0U) {
        return -1;
    }

    if (cc_read_status(CC_PARTNUM, &part) != 0 ||
        cc_read_status(CC_VERSION, &ver) != 0) {
        return -1;
    }
    if (part != 0x00U || ver == 0x00U || ver == 0xFFU) {
        return -1;
    }

    for (i = 0U; i < (uint8_t)(sizeof(s_cc_cfg) / sizeof(s_cc_cfg[0])); i++) {
        if (cc_write_reg(s_cc_cfg[i].reg, s_cc_cfg[i].val) != 0) {
            return -1;
        }
    }
    if (cc_write_reg(CC_PATABLE, 0xC4U) != 0) {
        return -1;
    }

    cc1101_sleep();
    return 0;
}

void cc1101_wake(void) {
    if (hal_spi_cs_low() != 0) {
        return;
    }
    (void)hal_spi_xfer(CC_SIDLE);
    hal_spi_cs_high();
    if (cc_enter_idle() != 0) {
        return;
    }

    /* Restore registers not retained across SPWD sleep. */
    (void)cc_write_reg(CC_TEST2, 0x81U);
    (void)cc_write_reg(CC_TEST1, 0x35U);
    (void)cc_write_reg(CC_TEST0, 0x09U);
    (void)cc_write_reg(CC_PATABLE, 0xC4U);
}

int cc1101_tx(const uint8_t frame[24]) {
    uint16_t step;

    if (cc_enter_idle() != 0 ||
        cc_strobe(CC_SFTX) != 0 ||
        cc_write_burst(CC_TXFIFO, frame, CC_FRAME_LEN) != 0 ||
        cc_strobe(CC_STX) != 0) {
        return -1;
    }

    /* Wait up to ~15 ms for GDO0 rising (sync word transmitted). */
    for (step = 0U; step < 150U; step++) {
        if (hal_gdo0_read() != 0U) {
            break;
        }
        hal_delay_us(100U);
        hal_wdt_feed();
    }
    if (step >= 150U) {
        (void)cc_enter_idle();
        (void)cc_strobe(CC_SFTX);
        return -1;
    }

    /* Wait up to ~20 ms for GDO0 falling (payload + CRC complete). */
    for (step = 0U; step < 200U; step++) {
        if (hal_gdo0_read() == 0U) {
            break;
        }
        hal_delay_us(100U);
        hal_wdt_feed();
    }
    if (step >= 200U) {
        (void)cc_enter_idle();
        (void)cc_strobe(CC_SFTX);
        return -1;
    }

    (void)cc_enter_idle();
    return 0;
}

int cc1101_rx(uint8_t frame[24], uint16_t timeout_ms) {
    uint16_t rem_ms = timeout_ms;
    uint8_t frame_tries;
    uint8_t buf[CC_FRAME_LEN + 2U];

    for (frame_tries = 0U; frame_tries < 6U && rem_ms > 0U; frame_tries++) {
        uint16_t ms;
        uint8_t sub;
        uint8_t saw_high = 0U;
        uint8_t gdo0_fell = 0U;
        uint8_t rxbytes = 0U;
        uint8_t i;

        if (cc_enter_idle() != 0 ||
            cc_strobe(CC_SFRX) != 0 ||
            cc_strobe(CC_SRX) != 0) {
            return 0;
        }

        while (rem_ms > 0U) {
            rem_ms--;
            for (sub = 0U; sub < 10U; sub++) {
                if (hal_gdo0_read() != 0U) {
                    saw_high = 1U;
                    break;
                }
                hal_delay_us(100U);
            }
            hal_wdt_feed();
            if (saw_high != 0U) {
                break;
            }
        }

        if (saw_high == 0U) {
            (void)cc_enter_idle();
            (void)cc_strobe(CC_SFRX);
            return 0;
        }

        /* Extend up to +15 ms while GDO0 stays HIGH to let an in-flight frame finish. */
        for (ms = 0U; ms < 15U; ms++) {
            for (sub = 0U; sub < 10U; sub++) {
                if (hal_gdo0_read() == 0U) {
                    gdo0_fell = 1U;
                    break;
                }
                hal_delay_us(100U);
            }
            hal_wdt_feed();
            if (gdo0_fell != 0U) {
                break;
            }
        }
        if (rem_ms > ms) {
            rem_ms = (uint16_t)(rem_ms - ms);
        } else {
            rem_ms = 0U;
        }

        if (gdo0_fell == 0U) {
            (void)cc_enter_idle();
            (void)cc_strobe(CC_SFRX);
            continue;
        }

        if (cc_read_status_safe(CC_RXBYTES, &rxbytes) != 0) {
            (void)cc_enter_idle();
            (void)cc_strobe(CC_SFRX);
            return 0;
        }
        if ((rxbytes & 0x80U) != 0U || (rxbytes & 0x7FU) != (CC_FRAME_LEN + 2U)) {
            (void)cc_enter_idle();
            (void)cc_strobe(CC_SFRX);
            continue;
        }

        if (cc_read_burst(CC_RXFIFO, buf, (uint8_t)(CC_FRAME_LEN + 2U)) != 0) {
            (void)cc_enter_idle();
            (void)cc_strobe(CC_SFRX);
            return 0;
        }
        (void)cc_enter_idle();
        (void)cc_strobe(CC_SFRX);

        if ((buf[CC_FRAME_LEN + 1U] & 0x80U) == 0U) {
            continue;
        }

        for (i = 0U; i < CC_FRAME_LEN; i++) {
            frame[i] = buf[i];
        }
        return 1;
    }

    (void)cc_enter_idle();
    (void)cc_strobe(CC_SFRX);
    return 0;
}

void cc1101_sleep(void) {
    if (cc_enter_idle() != 0) {
        return;
    }
    (void)cc_strobe(CC_SFRX);
    (void)cc_strobe(CC_SFTX);
    (void)cc_strobe(CC_SPWD);
}

