/**
 * @file pic_stubs.h
 * @brief The camera's picture path in the host tests: what bf_send_picture() and pic_ctr_ok()
 *        (cut out of MD_CAM/firmware/main.c by extract_ctr.py) call, stubbed.
 *
 * transmit() keeps the counter part of the real one (MD_CAM/firmware/main.c): the rule
 * ctr_use(ctr >> 8), s_tx_held, and s_ctr + 1 only for a packet that is sent. The radio, the
 * Blackfin link and the delays are replaced by a log of what was sent and a scripted Blackfin
 * that has a picture of pic_len bytes ready at once.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ctr_dev.h"

#define RF_CH_MAIN 0
#define RF_CH_PCH  1

static struct pic_trace s_pic;
static uint16_t s_bf_len = 100;   /* length of the Blackfin's picture */
static int s_bf_state;            /* 0 idle, 1 length low, 2 length high, 3 data */
static bool s_pch = false;
static uint8_t s_pic_ch = RF_CH_MAIN;
static bool s_token_enabled = false;
static bool s_pic_has_token = false;

static void radio_set_freq(uint8_t ch) { (void)ch; }

static void clock_xosc(void) {}

static void delay_ms(uint16_t ms) { (void)ms; }

static bool cam_token_acquire(uint8_t cmd, bool motion, bool *has_token) {
    (void)cmd;
    (void)motion;
    *has_token = false;
    return true;
}

static uint32_t transmit(bool is_motion, bool is_por, uint8_t marker, uint8_t arg) {
    uint32_t ctr = s_ctr;
    (void)is_motion;
    (void)is_por;
    (void)arg;
    s_tx_held = !ctr_use(ctr >> 8);
    if (s_tx_held)
        return ctr;
    if (s_pic.npackets < PIC_TRACE_MAX) {
        s_pic.packet_ctr[s_pic.npackets] = ctr;
        s_pic.packet_marker[s_pic.npackets] = marker;
    }
    s_pic.npackets++;
    s_ctr = ctr + 1;
    return ctr;
}

static uint8_t bf_link_xfer(uint8_t tx) {
    if (tx >= 0xD0 && tx <= 0xD6) {
        s_pic.bf_cmds++;
        s_bf_state = 0;
        return 0;
    }
    if (tx == 0xA0) {             /* the status after 0xB0: ready */
        s_bf_state = 1;
        return 0x5A;
    }
    if (tx != 0x00)
        return 0;
    switch (s_bf_state) {
    case 1: s_bf_state = 2; return (uint8_t)s_bf_len;
    case 2: s_bf_state = 3; return (uint8_t)(s_bf_len >> 8);
    case 3: return 0x42;
    default: return 0;
    }
}

static void transmit_big_chunk(uint8_t type, uint16_t chunk_idx, uint8_t chunk_len,
                               bool is_first, bool is_last, const uint8_t *data) {
    (void)type;
    (void)chunk_idx;
    (void)chunk_len;
    (void)is_first;
    (void)is_last;
    (void)data;
    if (s_pic.nchunks < PIC_TRACE_MAX)
        s_pic.chunk_tag[s_pic.nchunks] = s_pic_tag;
    s_pic.nchunks++;
}
