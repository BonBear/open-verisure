/**
 * @file main.c
 * @brief OpenVerisure SIREN Phase 2 Firmware (MSP430F149 Rev S + CC1101 26 MHz).
 *
 * Phase 2 scope: persistent counter, AES-128-ECB uplink telemetry (boot 0x00,
 * tamper 0xB5, poll 0xBA), 120 ms downlink RX validation (DL...VS), LPM3
 * low-power sleep, and OKI L22901-707 (IC2) parallel audio driver on P6.0..P6.5
 * with audio rail (P1.4). HV horn boost (P1.7) remains Hi-Z INPUT (OFF) unless
 * SIREN_ENABLE_HV_BOOST is explicitly defined at compile time.
 */

#include <msp430.h>
#include <stdint.h>
#include "hal_msp430.h"
#include "cc1101.h"
#include "aes128.h"
#include "flash_ctr.h"

#ifndef __code
#define __code
#endif

#ifdef SIREN_USE_EXAMPLE_CONFIG
#include "config.h.example"
#else
#include "config.h"
#endif

#ifndef DEVICE_ID_STR
#ifdef DEVICE_ID
#define DEVICE_ID_STR DEVICE_ID
#else
#error "DEVICE_ID_STR (or DEVICE_ID) must be defined in config.h"
#endif
#endif

typedef char siren_dev_id_len_check[(sizeof(DEVICE_ID_STR) >= 9U) ? 1 : -1];

#ifndef CTR_SEED
#define CTR_SEED 1UL
#endif

#ifndef POLL_DISARMED_S
#define POLL_DISARMED_S 120U
#endif

#ifndef POLL_ARMED_S
#define POLL_ARMED_S 30U
#endif

#define POLL_SOUNDING_S       5U

#define SIREN_CMD_OFF         0xA0U
#define SIREN_CMD_ON          0xA1U
#define SIREN_DEFAULT_SOUND_S 60U
#define SIREN_MAX_SOUND_S     180U

#define SIREN_IC2_CMD_STOP     0x01U
#define SIREN_IC2_CMD_BEEP2    0x04U
#define SIREN_IC2_CMD_TONE     0x08U
#define SIREN_IC2_CMD_CHIME2   0x0CU
#define SIREN_IC2_CMD_ALARM    0x14U
#define SIREN_IC2_CMD_DINGDONG 0x18U
#define SIREN_IC2_CMD_BEEP1    0x1CU
#define SIREN_IC2_RETRIG_S     2U

#define BOOT_MIN_MV      2700U
#define BOOT_RECHECK_S   60U
#define RX_WINDOW_MS     120U

#define TAMPER_MIN_GAP_S 2U
#define TAMPER_MAX_GAP_S 300U
#define TAMPER_CALM_S    600U

#define PKT_TYPE_ANNOUNCE 0x00U
#define PKT_TYPE_TAMPER   0xB5U
#define PKT_TYPE_TEMP     0xB6U
#define PKT_TYPE_POLL     0xBAU
#define TEMP_ENC_INVALID  0xFFU
#define TEMP_POLL_INTERVAL 10U

static uint8_t s_rst_cause = 0U;
static uint8_t s_armed = 0U;
static uint8_t s_tamper = 0U;
static uint8_t s_sounding = 0U;
static uint16_t s_sound_rem_s = 0U;
static uint16_t s_ic2_retrig_s = 0U;
static uint8_t s_sound_cmd = SIREN_IC2_CMD_ALARM;
static uint8_t s_sound_repeat = 0U;
static uint8_t s_cmd_ack = 0U;
static uint8_t s_first_pkt_sent = 0U;
static uint8_t s_last_temp_enc = TEMP_ENC_INVALID;
static uint8_t s_temp_poll_cnt = 0U;
static volatile uint8_t s_latched_cmd = 0U;

static void wipe_bytes(volatile uint8_t *p, uint8_t len) {
    uint8_t i;
    for (i = 0U; i < len; i++) {
        p[i] = 0U;
    }
}

static uint8_t calc_battery_pct(uint16_t mv) {
    if (mv >= 3000U) {
        return 100U;
    }
    if (mv <= 2200U) {
        return 0U;
    }
    return (uint8_t)(((uint32_t)(mv - 2200U) * 100UL) / 800UL);
}

typedef struct {
    uint16_t adc;
    int16_t c10;
} th1_lut_t;

static uint8_t encode_th1_temp(uint16_t adc) {
    /* 10 kOhm B=3950 NTC to BAT+ with ~68 kOhm pull-down to GND (12-bit ratiometric).
     * Calibrated from bench measurement (adc ~3503 at ~22 C room temperature).
     */
    static const th1_lut_t lut[] = {
        {  593U, -400 }, { 1038U, -300 }, { 1606U, -200 }, { 2206U, -100 },
        { 2740U,    0 }, { 3158U,  100 }, { 3458U,  200 }, { 3570U,  250 },
        { 3662U,  300 }, { 3799U,  400 }, { 3890U,  500 }, { 3951U,  600 },
        { 3992U,  700 }, { 4031U,  850 }
    };
    uint8_t i;
    int16_t t_c10;
    uint16_t enc;

    if (adc < 32U || adc > 4063U) {
        return TEMP_ENC_INVALID;
    }
    if (adc <= lut[0].adc) {
        t_c10 = lut[0].c10;
    } else if (adc >= lut[(sizeof(lut) / sizeof(lut[0])) - 1U].adc) {
        t_c10 = lut[(sizeof(lut) / sizeof(lut[0])) - 1U].c10;
    } else {
        t_c10 = lut[0].c10;
        for (i = 0U; i < (uint8_t)((sizeof(lut) / sizeof(lut[0])) - 1U); i++) {
            if (adc <= lut[i + 1U].adc) {
                uint16_t da = (uint16_t)(lut[i + 1U].adc - lut[i].adc);
                int32_t num = (int32_t)(adc - lut[i].adc) * (int32_t)(lut[i + 1U].c10 - lut[i].c10);
                t_c10 = (int16_t)(lut[i].c10 + (int16_t)(num / (int32_t)da));
                break;
            }
        }
    }
    enc = (uint16_t)(t_c10 + 400) / 5U;
    return (enc > 254U) ? 254U : (uint8_t)enc;
}

static void sleep_seconds(uint16_t sec) {
    while (sec > 0U) {
        uint16_t elapsed;
        hal_wdt_hold();
        hal_sleep_lpm3(hal_read_sw1());
        hal_wdt_arm_active();

        __disable_interrupt();
        __nop();
        elapsed = g_ticks;
        g_ticks = 0U;
        g_tamper_irq = 0U;
        __enable_interrupt();

        if (elapsed >= sec) {
            break;
        }
        sec = (uint16_t)(sec - elapsed);
    }
}

static void boot_battery_guard(void) {
    while (hal_read_batt_mv() < BOOT_MIN_MV) {
        hal_led_set(0U);
        sleep_seconds(BOOT_RECHECK_S);
    }
}

static void build_uplink_frame(uint32_t ctr, uint16_t batt_mv, uint8_t is_por,
                               uint8_t pkt_type, uint8_t pkt_arg, uint8_t frame[24]) {
    uint8_t plain[16];
    uint8_t flags = 0x20U; /* Persistent 32-bit counter format */
    uint8_t i;

    for (i = 0U; i < 8U; i++) {
        frame[i] = (uint8_t)DEVICE_ID_STR[i];
    }

    if (s_sounding != 0U) {
        flags |= 0x01U;
    }
    if (s_tamper != 0U) {
        flags |= 0x04U;
    }
    if (is_por != 0U) {
        flags |= 0x08U;
    }
    if (batt_mv < BOOT_MIN_MV) {
        flags |= 0x10U;
    }
    flags |= (uint8_t)((s_rst_cause & 0x03U) << 6);

    plain[0]  = (uint8_t)(ctr >> 8);
    plain[1]  = (uint8_t)(ctr);
    plain[2]  = flags;
    plain[3]  = calc_battery_pct(batt_mv);
    plain[4]  = (uint8_t)(batt_mv >> 8);
    plain[5]  = (uint8_t)(batt_mv & 0xFFU);
    plain[6]  = (uint8_t)DEVICE_ID_STR[0];
    plain[7]  = (uint8_t)DEVICE_ID_STR[1];
    plain[8]  = (uint8_t)DEVICE_ID_STR[2];
    plain[9]  = (uint8_t)DEVICE_ID_STR[3];
    plain[10] = (uint8_t)'V';
    plain[11] = (uint8_t)'S';
    plain[12] = pkt_type;
    plain[13] = pkt_arg;
    plain[14] = (uint8_t)(ctr >> 16);
    plain[15] = (uint8_t)(ctr >> 24);

    hal_wdt_feed();
    aes128_ecb_encrypt(DEVICE_AES_KEY, plain, &frame[8]);
    wipe_bytes(plain, (uint8_t)sizeof(plain));
}

/* Latch 5-bit command onto IC2 (P6.5, P6.3..P6.0) via falling edge on P6.4. */
static void siren_ic2_strobe(uint8_t val5) {
    uint8_t p6_bits = (uint8_t)(((val5 & 0x10U) << 1) | (val5 & 0x0FU));
    P6OUT = (uint8_t)((P6OUT & (uint8_t)~0x3FU) | p6_bits | BIT4);
    hal_wdt_feed();
    hal_delay_ms(10U);
    P6OUT &= (uint8_t)~BIT4;
}

static void siren_audio_set(uint8_t on) {
    if (on != 0U) {
        P1OUT &= (uint8_t)~BIT4;
#ifdef SIREN_ENABLE_HV_BOOST
        P1OUT |= BIT7;
        P1DIR |= BIT7;
#else
        P1DIR &= (uint8_t)~BIT7;
        P1OUT &= (uint8_t)~BIT7;
#endif
        hal_delay_ms(20U);
        siren_ic2_strobe(s_sound_cmd);
        s_ic2_retrig_s = 0U;
    } else {
        P1DIR &= (uint8_t)~BIT7;
        P1OUT &= (uint8_t)~BIT7;
        s_ic2_retrig_s = 0U;
        if ((P1OUT & BIT4) == 0U) {
            siren_ic2_strobe(SIREN_IC2_CMD_STOP);
            hal_wdt_feed();
            hal_delay_ms(60U);
            P6OUT &= (uint8_t)~0x3FU;
        }
        P1OUT |= BIT4;
    }
}

static void siren_handle_downlink_cmd(uint8_t cmd, uint8_t arg, uint8_t tone) {
    static const uint8_t s_tone_cmds[6] = {
        SIREN_IC2_CMD_ALARM,     /* 0: alarm */
        SIREN_IC2_CMD_DINGDONG,  /* 1: ding_dong */
        SIREN_IC2_CMD_CHIME2,    /* 2: chime */
        SIREN_IC2_CMD_BEEP2,     /* 3: double_beep */
        SIREN_IC2_CMD_BEEP1,     /* 4: beep */
        SIREN_IC2_CMD_TONE       /* 5: tone */
    };
    s_latched_cmd = cmd;
    if (cmd == SIREN_CMD_ON) {
        uint8_t t = (tone <= 5U) ? tone : 0U;
        uint16_t dur;
        s_sound_cmd = s_tone_cmds[t];
        if (t == 0U) {
            dur = (arg == 0U) ? SIREN_DEFAULT_SOUND_S : (uint16_t)arg;
            s_sound_repeat = 1U;
        } else {
            dur = (arg == 0U) ? ((t == 1U) ? 2U : 1U) : (uint16_t)arg;
            s_sound_repeat = (dur > SIREN_IC2_RETRIG_S) ? 1U : 0U;
        }
        if (dur > SIREN_MAX_SOUND_S) {
            dur = SIREN_MAX_SOUND_S;
        }
        s_sounding = 1U;
        s_sound_rem_s = dur;
        siren_audio_set(1U);
    } else if (cmd == SIREN_CMD_OFF) {
        s_sounding = 0U;
        s_sound_rem_s = 0U;
        s_sound_repeat = 0U;
        siren_audio_set(0U);
    }
}

static uint8_t validate_downlink(const uint8_t rx[24], uint32_t poll_ctr) {
    uint8_t dl_plain[16];
    uint32_t dl_ctr;
    uint8_t i;

    for (i = 0U; i < 8U; i++) {
        if (rx[i] != (uint8_t)DEVICE_ID_STR[i]) {
            return 0U;
        }
    }

    hal_wdt_feed();
    aes128_ecb_decrypt(DEVICE_AES_KEY, &rx[8], dl_plain);
    if (dl_plain[0] != (uint8_t)'D' || dl_plain[1] != (uint8_t)'L' ||
        dl_plain[14] != (uint8_t)'V' || dl_plain[15] != (uint8_t)'S') {
        wipe_bytes(dl_plain, (uint8_t)sizeof(dl_plain));
        return 0U;
    }

    dl_ctr = ((uint32_t)dl_plain[2] << 24) |
             ((uint32_t)dl_plain[3] << 16) |
             ((uint32_t)dl_plain[4] << 8)  |
             (uint32_t)dl_plain[5];
    if (dl_ctr != poll_ctr) {
        wipe_bytes(dl_plain, (uint8_t)sizeof(dl_plain));
        return 0U;
    }

    s_cmd_ack = 0U;
    if (dl_plain[6] <= 1U) {
        s_armed = (dl_plain[6] != 0U) ? 1U : 0U;
    }
    if (dl_plain[7] != 0U) {
        siren_handle_downlink_cmd(dl_plain[7], dl_plain[8], dl_plain[9]);
        s_cmd_ack = 1U;
    }

    wipe_bytes(dl_plain, (uint8_t)sizeof(dl_plain));
    return 1U;
}

static uint8_t siren_poll_state(void) {
    uint8_t st = 0U;
    if (s_armed != 0U) {
        st |= 0x01U;
    }
    if (s_sounding != 0U) {
        st |= 0x02U;
    }
    if (s_cmd_ack != 0U) {
        st |= 0x04U;
    }
    return st;
}

static void blink_tx_led(void) {
    hal_led_set(1U);
    hal_delay_ms(20U);
    hal_led_set(0U);
}

static int send_packet(uint8_t pkt_type, uint8_t pkt_arg, uint8_t listen_dl) {
    uint16_t batt_mv = hal_read_batt_mv();
    uint32_t ctr = 0UL;
    uint8_t frame[24];
    uint8_t is_por = 0U;
    int tx_ok;

    if (ctr_next(batt_mv, &ctr) != 0) {
        return -1;
    }

    if (s_first_pkt_sent == 0U && s_rst_cause == 0U) {
        is_por = 1U;
    }
    s_first_pkt_sent = 1U;

    build_uplink_frame(ctr, batt_mv, is_por, pkt_type, pkt_arg, frame);

    cc1101_wake();
    tx_ok = cc1101_tx(frame);
    if (tx_ok == 0 && listen_dl != 0U) {
        uint8_t rx_frame[24];
        if (cc1101_rx(rx_frame, RX_WINDOW_MS) > 0) {
            (void)validate_downlink(rx_frame, ctr);
        }
    }
    cc1101_sleep();

    if (tx_ok == 0) {
        blink_tx_led();
    }
    return tx_ok;
}

static void poll_with_ack_flush(void) {
    (void)send_packet(PKT_TYPE_POLL, siren_poll_state(), 1U);
    if (s_cmd_ack != 0U) {
        (void)send_packet(PKT_TYPE_POLL, siren_poll_state(), 1U);
    }
}

static void send_temp_if_needed(uint8_t force) {
    uint8_t enc = encode_th1_temp(hal_read_th1_adc());
    uint8_t diff;
    if (enc == TEMP_ENC_INVALID) {
        return;
    }
    if (force == 0U && s_last_temp_enc != TEMP_ENC_INVALID) {
        diff = (enc >= s_last_temp_enc)
                   ? (uint8_t)(enc - s_last_temp_enc)
                   : (uint8_t)(s_last_temp_enc - enc);
        if (s_temp_poll_cnt < TEMP_POLL_INTERVAL && diff < 2U) {
            return;
        }
    }
    s_temp_poll_cnt = 0U;
    if (send_packet(PKT_TYPE_TEMP, enc, 0U) == 0) {
        s_last_temp_enc = enc;
    }
}

int main(void) {
    uint8_t i;
    uint16_t poll_ticks = 0U;
    uint16_t tamper_gap = 0U;
    uint16_t tamper_backoff = TAMPER_MIN_GAP_S;
    uint16_t tamper_calm = 0U;

    hal_init();
    s_rst_cause = hal_reset_cause();
    hal_wdt_arm_active();

    if (s_rst_cause == 2U) {
        sleep_seconds(5U);
    }

    boot_battery_guard();

    while (cc1101_init() != 0) {
        hal_led_set(0U);
        sleep_seconds(BOOT_RECHECK_S);
    }

    ctr_boot(CTR_SEED);

    for (i = 0U; i < 3U; i++) {
        hal_led_set(1U);
        hal_delay_ms(50U);
        hal_led_set(0U);
        hal_delay_ms(80U);
    }

    s_tamper = hal_read_sw1();
    (void)send_packet(PKT_TYPE_ANNOUNCE, 0U, 0U);
    send_temp_if_needed(1U);
    poll_with_ack_flush();

    for (;;) {
        uint16_t elapsed;
        uint8_t irq_flag;
        uint16_t poll_interval;

        hal_wdt_hold();
        hal_sleep_lpm3(hal_read_sw1());
        hal_wdt_arm_active();

        __disable_interrupt();
        __nop();
        elapsed = g_ticks;
        irq_flag = g_tamper_irq;
        g_ticks = 0U;
        g_tamper_irq = 0U;
        __enable_interrupt();

        if (elapsed > 0U) {
            if ((uint32_t)poll_ticks + elapsed > 65535UL) {
                poll_ticks = 65535U;
            } else {
                poll_ticks = (uint16_t)(poll_ticks + elapsed);
            }

            if (tamper_gap > elapsed) {
                tamper_gap = (uint16_t)(tamper_gap - elapsed);
            } else {
                tamper_gap = 0U;
            }

            if (tamper_calm < TAMPER_CALM_S) {
                if ((uint32_t)tamper_calm + elapsed >= TAMPER_CALM_S) {
                    tamper_calm = TAMPER_CALM_S;
                    tamper_backoff = TAMPER_MIN_GAP_S;
                } else {
                    tamper_calm = (uint16_t)(tamper_calm + elapsed);
                }
            }

            if (s_sounding != 0U) {
                if (s_sound_rem_s > elapsed) {
                    s_sound_rem_s = (uint16_t)(s_sound_rem_s - elapsed);
                    if (s_sound_repeat != 0U) {
                        if ((uint32_t)s_ic2_retrig_s + elapsed >= SIREN_IC2_RETRIG_S) {
                            s_ic2_retrig_s = 0U;
                            siren_ic2_strobe(s_sound_cmd);
                        } else {
                            s_ic2_retrig_s = (uint16_t)(s_ic2_retrig_s + elapsed);
                        }
                    }
                    hal_led_set(1U);
                    hal_delay_ms(30U);
                    hal_led_set(0U);
                } else {
                    s_sound_rem_s = 0U;
                    s_sounding = 0U;
                    s_sound_repeat = 0U;
                    siren_audio_set(0U);
                    poll_ticks = 0U;
                    poll_with_ack_flush();
                }
            }
        }

        if (tamper_gap == 0U && (irq_flag != 0U || hal_read_sw1() != s_tamper)) {
            uint8_t sw_now;
            hal_delay_ms(50U);
            sw_now = hal_read_sw1();
            if (sw_now != s_tamper) {
                s_tamper = sw_now;
                tamper_gap = tamper_backoff;
                tamper_backoff = (tamper_backoff <= (TAMPER_MAX_GAP_S / 2U))
                                     ? (uint16_t)(tamper_backoff * 2U)
                                     : TAMPER_MAX_GAP_S;
                tamper_calm = 0U;
                (void)send_packet(PKT_TYPE_TAMPER, s_tamper ? 1U : 0U, 0U);
            }
        }

        if (s_sounding != 0U) {
            poll_interval = POLL_SOUNDING_S;
        } else if (s_armed != 0U) {
            poll_interval = POLL_ARMED_S;
        } else {
            poll_interval = POLL_DISARMED_S;
        }
        if (poll_ticks >= poll_interval) {
            poll_ticks = 0U;
            poll_with_ack_flush();
            if (s_sounding == 0U) {
                s_temp_poll_cnt++;
                send_temp_if_needed(0U);
            }
        }
    }
}

