/**
 * @file hal_msp430.c
 * @brief MSP430F149 Rev S HAL: pin safety, 32.768 kHz LFXT1 + DCO FLL, LPM3 Timer_A,
 *        USART1 SPI Mode 0, ADC12 (AVCC/2 & TH1), SW1 tamper IRQ, and WDT.
 */

#include "hal_msp430.h"
#include <msp430.h>

volatile uint16_t g_ticks = 0U;
volatile uint8_t g_tamper_irq = 0U;
uint8_t g_xtal_fault = 0U;

static uint8_t s_reset_cause = 0U;

static void gpio_init_safe(void) {
    /* Port 1: P1.7 (IC9 boost) = OUT 0, P1.4 (Q3 audio) = OUT 0, P1.6 (SW1) = IN,
     * all other P1 pins = OUT 0. MSP430F149 has no PxREN; never drive P1.6 HIGH. */
    P1IE = 0x00U;
    P1IFG = 0x00U;
    P1SEL = 0x00U;
    P1OUT = 0x00U;
    P1DIR = (uint8_t)~BIT6;

    /* Port 2: P2.6 (GDO0) & P2.7 (GDO2) = inputs only; P2.0..P2.5 = OUT 0. */
    P2IE = 0x00U;
    P2IFG = 0x00U;
    P2SEL = 0x00U;
    P2OUT = 0x00U;
    P2DIR = (uint8_t)~(BIT6 | BIT7);

    /* Port 3: all unmapped = OUT 0. */
    P3SEL = 0x00U;
    P3OUT = 0x00U;
    P3DIR = 0xFFU;

    /* Port 4: P4.7 (LED1 active-high via Q1) = OUT 0; P4.0..P4.6 = OUT 0. */
    P4SEL = 0x00U;
    P4OUT = 0x00U;
    P4DIR = 0xFFU;

    /* Port 5: P5.0 (CSn) = OUT 1; P5.1 (SIMO1) & P5.3 (UCLK1) = periph out;
     * P5.2 (SOMI1) = periph in; P5.4..P5.7 = OUT 0. */
    P5OUT = BIT0;
    P5DIR = (uint8_t)~BIT2;
    P5SEL = BIT1 | BIT2 | BIT3;

    /* Port 6: analog mode on all pins (0 uA digital buffer draw); A7 (P6.7) = TH1. */
    P6SEL = 0xFFU;
    P6DIR = 0x00U;
}

static void spi_init(void) {
    U1CTL = CHAR | SYNC | MM | SWRST;
    U1TCTL = CKPH | SSEL1 | STC;
    U1RCTL = 0x00U;
    U1MCTL = 0x00U;
    U1BR0 = 2U;
    U1BR1 = 0x00U;
    ME2 |= USPIE1;
    U1CTL &= (uint8_t)~SWRST;
}

static void clocks_init(void) {
    uint8_t stable = 0U;
    uint8_t iter;

    BCSCTL1 = XT2OFF | RSEL2;
    BCSCTL2 = 0x00U;
    DCOCTL = 0x60U;

    /* Wait up to 2000 ms (40 x 50 ms) for 32.768 kHz LFXT1 crystal. */
    g_xtal_fault = 1U;
    for (iter = 0U; iter < 40U; iter++) {
        IFG1 &= (uint8_t)~OFIFG;
        hal_delay_ms(50U);
        if ((IFG1 & OFIFG) == 0U) {
            stable++;
            if (stable >= 4U) {
                g_xtal_fault = 0U;
                break;
            }
        } else {
            stable = 0U;
        }
    }

    if (g_xtal_fault == 0U) {
        /* Software FLL: target 256 SMCLK cycles per ACLK/8 edge (1.048576 MHz +/- 1%). */
        uint8_t fll_iter;
        BCSCTL1 |= DIVA_3;
        TACTL = TASSEL_2 | MC_2 | TACLR;
        TACCTL2 = CM_1 | CCIS_1 | SCS | CAP;

        for (fll_iter = 0U; fll_iter < 64U; fll_iter++) {
            uint16_t c1;
            uint16_t c2;
            uint16_t delta;
            uint16_t to;
            uint8_t step;

            TACCTL2 &= (uint16_t)~CCIFG;
            to = 20000U;
            while ((TACCTL2 & CCIFG) == 0U && --to != 0U) {
            }
            if (to == 0U) {
                break;
            }
            c1 = TACCR2;

            TACCTL2 &= (uint16_t)~CCIFG;
            to = 20000U;
            while ((TACCTL2 & CCIFG) == 0U && --to != 0U) {
            }
            if (to == 0U) {
                break;
            }
            c2 = TACCR2;

            delta = (uint16_t)(c2 - c1);
            if (delta >= 254U && delta <= 258U) {
                break;
            }
            if (delta < 254U) {
                step = (delta < 224U) ? 8U : ((delta < 246U) ? 2U : 1U);
                if ((uint16_t)DCOCTL + step <= 0xFFU) {
                    DCOCTL = (uint8_t)(DCOCTL + step);
                } else if ((BCSCTL1 & 0x07U) < 0x07U) {
                    BCSCTL1 = (uint8_t)(BCSCTL1 + 1U);
                    DCOCTL = 0x20U;
                } else {
                    DCOCTL = 0xFFU;
                    break;
                }
            } else {
                step = (delta > 288U) ? 8U : ((delta > 266U) ? 2U : 1U);
                if (DCOCTL >= step) {
                    DCOCTL = (uint8_t)(DCOCTL - step);
                } else if ((BCSCTL1 & 0x07U) > 0x00U) {
                    BCSCTL1 = (uint8_t)(BCSCTL1 - 1U);
                    DCOCTL = 0xE0U;
                } else {
                    DCOCTL = 0x00U;
                    break;
                }
            }
        }

        TACCTL2 = 0x0000U;
        TACTL = 0x0000U;
        BCSCTL1 &= (uint8_t)~DIVA_3;
    }
}

static void timer_init(void) {
    if (g_xtal_fault == 0U) {
        /* 32768 Hz / 8 = 4096 Hz; 4096 ticks (0..4095) = 1.000 s. */
        TACTL = TASSEL_1 | ID_3 | MC_1 | TACLR;
        TACCR0 = 4095U;
        TACCTL0 = CCIE;
    } else {
        /* LPM0 fallback on SMCLK (~1.048576 MHz / 8 / 65536 = 2 Hz; /2 in ISR). */
        TACTL = TASSEL_2 | ID_3 | MC_1 | TACLR;
        TACCR0 = 65535U;
        TACCTL0 = CCIE;
    }
}

void hal_init(void) {
    if ((IFG1 & WDTIFG) != 0U) {
        s_reset_cause = 2U;
#ifdef PORIFG
    } else if ((IFG1 & PORIFG) != 0U) {
        s_reset_cause = 0U;
#endif
#ifdef RSTIFG
    } else if ((IFG1 & RSTIFG) != 0U) {
        s_reset_cause = 1U;
#endif
    } else {
        s_reset_cause = 3U;
    }

    hal_wdt_hold();
    IFG1 &= (uint8_t)~(WDTIFG | OFIFG);
#ifdef RSTIFG
    IFG1 &= (uint8_t)~RSTIFG;
#endif
#ifdef PORIFG
    IFG1 &= (uint8_t)~PORIFG;
#endif

    gpio_init_safe();
    clocks_init();
    spi_init();
    timer_init();
    hal_sw1_arm_irq(hal_read_sw1());
    __enable_interrupt();
}

void hal_sleep_lpm3(uint8_t expected_sw1) {
    __disable_interrupt();
    __nop();
    hal_sw1_arm_irq(expected_sw1);
    if (g_ticks == 0U && g_tamper_irq == 0U && hal_read_sw1() == expected_sw1) {
        if (g_xtal_fault != 0U) {
            __bis_SR_register(LPM0_bits | GIE);
        } else {
            __bis_SR_register(LPM3_bits | GIE);
        }
        __nop();
    } else {
        __enable_interrupt();
    }
}

int hal_spi_cs_low(void) {
    uint16_t to;
    P5OUT &= (uint8_t)~BIT0;
    for (to = 0U; to < 500U; to++) {
        if ((P5IN & BIT2) == 0U) {
            return 0;
        }
        hal_delay_us(10U);
    }
    P5OUT |= BIT0;
    return -1;
}

void hal_spi_cs_high(void) {
    P5OUT |= BIT0;
}

uint8_t hal_spi_xfer(uint8_t b) {
    uint16_t to = 10000U;
    while ((IFG2 & UTXIFG1) == 0U && --to != 0U) {
    }
    U1TXBUF = b;
    to = 10000U;
    while ((IFG2 & URXIFG1) == 0U && --to != 0U) {
    }
    return U1RXBUF;
}

uint8_t hal_gdo0_read(void) {
    return (P2IN & BIT6) ? 1U : 0U;
}

uint8_t hal_spi_so_read(void) {
    return (P5IN & BIT2) ? 1U : 0U;
}

uint16_t hal_read_batt_mv(void) {
    uint16_t raw;
    uint16_t mv;
    uint16_t to;

    ADC12CTL0 = SHT0_8 | REFON | ADC12ON;
    ADC12CTL1 = SHP;
    ADC12MCTL0 = SREF_1 | INCH_11;
    hal_delay_ms(18U);

    ADC12IFG &= (uint16_t)~BIT0;
    ADC12CTL0 |= ENC | ADC12SC;
    to = 5000U;
    while ((ADC12IFG & BIT0) == 0U && --to != 0U) {
    }
    raw = ADC12MEM0 & 0x0FFFU;

    if (raw < 4000U) {
        mv = (uint16_t)(((uint32_t)raw * 3000UL + 2047UL) / 4095UL);
    } else {
        ADC12CTL0 &= (uint16_t)~ENC;
        ADC12CTL0 = SHT0_8 | REF2_5V | REFON | ADC12ON;
        hal_delay_ms(18U);

        ADC12IFG &= (uint16_t)~BIT0;
        ADC12CTL0 |= ENC | ADC12SC;
        to = 5000U;
        while ((ADC12IFG & BIT0) == 0U && --to != 0U) {
        }
        raw = ADC12MEM0 & 0x0FFFU;
        mv = (uint16_t)(((uint32_t)raw * 5000UL + 2047UL) / 4095UL);
    }

    ADC12CTL0 &= (uint16_t)~ENC;
    ADC12CTL0 = 0x0000U;
    ADC12CTL1 = 0x0000U;
    return mv;
}

uint16_t hal_read_th1_adc(void) {
    uint16_t raw;
    uint16_t to = 5000U;

    ADC12CTL0 = SHT0_4 | ADC12ON;
    ADC12CTL1 = SHP;
    ADC12MCTL0 = SREF_0 | INCH_7;
    ADC12IFG &= (uint16_t)~BIT0;
    ADC12CTL0 |= ENC | ADC12SC;
    while ((ADC12IFG & BIT0) == 0U && --to != 0U) {
    }
    raw = ADC12MEM0 & 0x0FFFU;

    ADC12CTL0 &= (uint16_t)~ENC;
    ADC12CTL0 = 0x0000U;
    ADC12CTL1 = 0x0000U;
    return raw;
}

void hal_delay_ms(uint16_t ms) {
    while (ms != 0U) {
        __delay_cycles(1049UL);
        ms--;
        if ((ms & 7U) == 0U) {
            hal_wdt_feed();
        }
    }
}

void hal_delay_us(uint16_t us) {
    while (us >= 10U) {
        __delay_cycles(10UL);
        us -= 10U;
    }
    while (us != 0U) {
        __nop();
        us--;
    }
}

void hal_led_set(uint8_t on) {
    if (on != 0U) {
        P4OUT |= BIT7;
    } else {
        P4OUT &= (uint8_t)~BIT7;
    }
}

uint8_t hal_read_sw1(void) {
    return (P1IN & BIT6) ? 1U : 0U;
}

void hal_sw1_arm_irq(uint8_t current_level) {
    P1IE &= (uint8_t)~BIT6;
    if (current_level != 0U) {
        P1IES |= BIT6;
    } else {
        P1IES &= (uint8_t)~BIT6;
    }
    P1IFG &= (uint8_t)~BIT6;
    P1IE |= BIT6;
}

void hal_wdt_hold(void) {
    WDTCTL = WDTPW | WDTHOLD;
}

void hal_wdt_arm_active(void) {
    if (g_xtal_fault == 0U) {
        WDTCTL = WDTPW | WDTCNTCL | WDTSSEL;
    } else {
        WDTCTL = WDTPW | WDTCNTCL;
    }
}

void hal_wdt_feed(void) {
    if ((WDTCTL & WDTHOLD) == 0U) {
        WDTCTL = WDTPW | WDTCNTCL | (WDTCTL & 0x00FFU);
    }
}

uint8_t hal_reset_cause(void) {
    return s_reset_cause;
}

__attribute__((interrupt(TIMERA0_VECTOR)))
void timera0_isr(void) {
    if (g_xtal_fault != 0U) {
        static uint8_t s_half = 0U;
        s_half ^= 1U;
        if (s_half != 0U) {
            return;
        }
    }
    g_ticks++;
    __bic_SR_register_on_exit(LPM3_bits);
}

__attribute__((interrupt(PORT1_VECTOR)))
void port1_isr(void) {
    P1IFG = 0x00U;
    g_tamper_irq = 1U;
    __bic_SR_register_on_exit(LPM3_bits);
}

