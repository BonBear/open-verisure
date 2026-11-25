/**
 * @file test_siren_firmware.c
 * @brief Host unit tests for OpenVerisure SIREN AES-128 and flash_ctr modules.
 *
 * Compile & run:
 *   gcc -Wall -Wextra -Werror -DHOST_TEST -I SIREN/firmware/src \
 *       tests/test_siren_firmware.c SIREN/firmware/src/aes128.c \
 *       SIREN/firmware/src/flash_ctr.c -o test_siren && ./test_siren
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "aes128.h"
#include "flash_ctr.h"
#include "cc1101.h"

static int g_failures = 0;

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
        return; \
    } \
} while (0)

#define ASSERT_EQ_U32(a, b) do { \
    uint32_t _va = (uint32_t)(a); \
    uint32_t _vb = (uint32_t)(b); \
    if (_va != _vb) { \
        fprintf(stderr, "FAIL %s:%d: 0x%08lX != 0x%08lX\n", \
                __FILE__, __LINE__, (unsigned long)_va, (unsigned long)_vb); \
        g_failures++; \
        return; \
    } \
} while (0)

#define ASSERT_EQ_INT(a, b) do { \
    int _va = (a); \
    int _vb = (b); \
    if (_va != _vb) { \
        fprintf(stderr, "FAIL %s:%d: %d != %d\n", __FILE__, __LINE__, _va, _vb); \
        g_failures++; \
        return; \
    } \
} while (0)

static void test_aes_fips197_appendix_b(void) {
    static const uint8_t key[16] = {
        0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
        0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C
    };
    static const uint8_t pt[16] = {
        0x32, 0x43, 0xF6, 0xA8, 0x88, 0x5A, 0x30, 0x8D,
        0x31, 0x31, 0x98, 0xA2, 0xE0, 0x37, 0x07, 0x34
    };
    static const uint8_t expected_ct[16] = {
        0x39, 0x25, 0x84, 0x1D, 0x02, 0xDC, 0x09, 0xFB,
        0xDC, 0x11, 0x85, 0x97, 0x19, 0x6A, 0x0B, 0x32
    };
    uint8_t ct[16];
    uint8_t dec[16];

    aes128_ecb_encrypt(key, pt, ct);
    ASSERT_TRUE(memcmp(ct, expected_ct, 16) == 0);

    aes128_ecb_decrypt(key, expected_ct, dec);
    ASSERT_TRUE(memcmp(dec, pt, 16) == 0);

    /* In-place encrypt and decrypt */
    memcpy(dec, pt, 16);
    aes128_ecb_encrypt(key, dec, dec);
    ASSERT_TRUE(memcmp(dec, expected_ct, 16) == 0);
    aes128_ecb_decrypt(key, dec, dec);
    ASSERT_TRUE(memcmp(dec, pt, 16) == 0);
}

static void test_aes_fips197_appendix_c1(void) {
    static const uint8_t key[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
    };
    static const uint8_t pt[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
    };
    static const uint8_t expected_ct[16] = {
        0x69, 0xC4, 0xE0, 0xD8, 0x6A, 0x7B, 0x04, 0x30,
        0xD8, 0xCD, 0xB7, 0x80, 0x70, 0xB4, 0xC5, 0x5A
    };
    uint8_t ct[16];
    uint8_t dec[16];

    aes128_ecb_encrypt(key, pt, ct);
    ASSERT_TRUE(memcmp(ct, expected_ct, 16) == 0);

    aes128_ecb_decrypt(key, expected_ct, dec);
    ASSERT_TRUE(memcmp(dec, pt, 16) == 0);
}

static void test_openverisure_kdf_and_packet_roundtrip(void) {
    static const uint8_t master_key[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
    };
    const char dev_id[8] = {'S', 'A', 'M', 'P', 'L', 'E', '0', '1'};
    uint8_t padded_id[16];
    uint8_t dev_key[16];
    uint8_t frame[24];
    uint8_t plain[16];
    uint8_t dec[16];
    uint32_t ctr = 0x01020304UL;
    uint16_t batt_mv = 2950U;
    uint8_t batt_pct = 93U;
    uint8_t flags = 0x28U; /* POR + persistent counter */
    uint16_t seq;
    uint32_t got_ctr;
    uint16_t got_mv;

    /* KDF: Device_Key = AES-128-ECB(Master_Key, Pad16(Device_ID)) */
    memset(padded_id, 0, sizeof(padded_id));
    memcpy(padded_id, dev_id, 8);
    aes128_ecb_encrypt(master_key, padded_id, dev_key);

    /* Uplink 24-byte packet layout matching 868Receiver/src/crypto.cpp */
    memcpy(frame, dev_id, 8);
    plain[0]  = (uint8_t)(ctr >> 8);
    plain[1]  = (uint8_t)(ctr);
    plain[2]  = flags;
    plain[3]  = batt_pct;
    plain[4]  = (uint8_t)(batt_mv >> 8);
    plain[5]  = (uint8_t)(batt_mv & 0xFFU);
    plain[6]  = (uint8_t)dev_id[0];
    plain[7]  = (uint8_t)dev_id[1];
    plain[8]  = (uint8_t)dev_id[2];
    plain[9]  = (uint8_t)dev_id[3];
    plain[10] = 'V';
    plain[11] = 'S';
    plain[12] = 0xBAU;
    plain[13] = 0x01U;
    plain[14] = (uint8_t)(ctr >> 16);
    plain[15] = (uint8_t)(ctr >> 24);

    aes128_ecb_encrypt(dev_key, plain, &frame[8]);

    /* Receiver decrypt & field checks */
    ASSERT_TRUE(memcmp(frame, dev_id, 8) == 0);
    aes128_ecb_decrypt(dev_key, &frame[8], dec);
    ASSERT_TRUE(dec[10] == 'V' && dec[11] == 'S');
    ASSERT_TRUE(memcmp(&dec[6], dev_id, 4) == 0);
    seq = ((uint16_t)dec[0] << 8) | dec[1];
    got_ctr = ((uint32_t)dec[15] << 24) | ((uint32_t)dec[14] << 16) | seq;
    got_mv = ((uint16_t)dec[4] << 8) | dec[5];
    ASSERT_EQ_U32(got_ctr, ctr);
    ASSERT_EQ_U32(got_mv, batt_mv);
    ASSERT_EQ_INT(dec[2], flags);
    ASSERT_EQ_INT(dec[3], batt_pct);
    ASSERT_EQ_INT(dec[12], 0xBA);
    ASSERT_EQ_INT(dec[13], 0x01);

    /* Downlink round-trip ('D','L', poll_ctr BE, armed, cmd, 'V','S') */
    memset(plain, 0, sizeof(plain));
    plain[0] = 'D';
    plain[1] = 'L';
    plain[2] = (uint8_t)(ctr >> 24);
    plain[3] = (uint8_t)(ctr >> 16);
    plain[4] = (uint8_t)(ctr >> 8);
    plain[5] = (uint8_t)(ctr);
    plain[6] = 1U;
    plain[7] = 0x00U;
    plain[14] = 'V';
    plain[15] = 'S';
    aes128_ecb_encrypt(dev_key, plain, &frame[8]);
    aes128_ecb_decrypt(dev_key, &frame[8], dec);
    ASSERT_TRUE(dec[0] == 'D' && dec[1] == 'L' && dec[14] == 'V' && dec[15] == 'S');
    got_ctr = ((uint32_t)dec[2] << 24) | ((uint32_t)dec[3] << 16) |
              ((uint32_t)dec[4] << 8) | (uint32_t)dec[5];
    ASSERT_EQ_U32(got_ctr, ctr);
    ASSERT_EQ_INT(dec[6], 1);
}

static void test_flash_ctr_blank_and_blocks(void) {
    uint32_t c = 0UL;
    uint16_t i;

    /* Seed 0 must clamp to >= 1 */
    flash_mock_reset();
    ctr_boot(0UL);
    ASSERT_EQ_INT(ctr_next(3000U, &c), 0);
    ASSERT_EQ_U32(c, 0x00000100UL);

    /* Blank boot with CTR_SEED = 5, advance across 256-step blocks */
    flash_mock_reset();
    ctr_boot(5UL);
    for (i = 0U; i < 256U; i++) {
        ASSERT_EQ_INT(ctr_next(3000U, &c), 0);
        ASSERT_EQ_U32(c, 0x00000500UL + i);
    }
    /* 257th packet rolls over to block 6 */
    ASSERT_EQ_INT(ctr_next(3000U, &c), 0);
    ASSERT_EQ_U32(c, 0x00000600UL);
    ASSERT_EQ_INT(ctr_next(3000U, &c), 0);
    ASSERT_EQ_U32(c, 0x00000601UL);

    /* Reboot skips +2 from newest logged block (6 -> 8) */
    ctr_boot(1UL);
    ASSERT_EQ_INT(ctr_next(3000U, &c), 0);
    ASSERT_EQ_U32(c, 0x00000800UL);
}

static void test_flash_ctr_ping_pong_rollover(void) {
    uint32_t c = 0UL;
    uint16_t blk;
    uint16_t step;

    flash_mock_reset();
    ctr_boot(1UL); /* Logs block 1 into seg 0 slot 0 */

    /* Advance through blocks 1..70 (crosses seg 0 [32 slots] -> seg 1 -> seg 0) */
    for (blk = 1U; blk <= 70U; blk++) {
        for (step = 0U; step < 256U; step++) {
            ASSERT_EQ_INT(ctr_next(3000U, &c), 0);
            ASSERT_EQ_U32(c, ((uint32_t)blk << 8) + step);
        }
    }

    /* Reboot must recover block 70 -> start at block 72 */
    ctr_boot(1UL);
    ASSERT_EQ_INT(ctr_next(3000U, &c), 0);
    ASSERT_EQ_U32(c, ((uint32_t)72UL << 8));
}

static void test_flash_ctr_cut_write_and_skip(void) {
    uint32_t c = 0UL;
    uint16_t i;

    flash_mock_reset();
    ctr_boot(10UL); /* Seg 0 slot 0 = block 10 */
    for (i = 0U; i < 256U; i++) {
        ASSERT_EQ_INT(ctr_next(3000U, &c), 0);
    }
    /* Next rollover logs block 11 into slot 1; inject 1 cut word1 -> slot 1 cut, slot 2 succeeds */
    g_mock_cut_word1 = 1;
    ASSERT_EQ_INT(ctr_next(3000U, &c), 0);
    ASSERT_EQ_U32(c, 0x00000B00UL);
    ASSERT_EQ_INT(g_mock_info[4U + 3U], 0xFF); /* Slot 1 has e[3] == 0xFF (cut word1) */

    /* Now simulate a power cut that stops all 3 write attempts on block 12 */
    for (i = 0U; i < 255U; i++) {
        ASSERT_EQ_INT(ctr_next(3000U, &c), 0);
    }
    g_mock_cut_word1 = 3;
    ASSERT_EQ_INT(ctr_next(3000U, &c), -1); /* Packet held on flash failure */
    g_mock_cut_word1 = 0;

    /* Reboot after power cut: skips cut slots 3..5, recovers block 11 -> resumes at 13 */
    ctr_boot(1UL);
    ASSERT_EQ_INT(ctr_next(3000U, &c), 0);
    ASSERT_EQ_U32(c, 0x00000D00UL);

    /* Non-blank slot skipping: dirty slot right after current entry */
    g_mock_info[7U * 4U] = 0xAAU;
    for (i = 1U; i < 256U; i++) {
        ASSERT_EQ_INT(ctr_next(3000U, &c), 0);
    }
    ASSERT_EQ_INT(ctr_next(3000U, &c), 0);
    ASSERT_EQ_U32(c, 0x00000E00UL);
    ASSERT_EQ_INT(g_mock_info[7U * 4U], 0xAA); /* Pre-dirty slot was not overwritten */
}

static void test_flash_ctr_low_batt_and_ceiling(void) {
    uint32_t c = 0UL;
    uint16_t i;
    uint8_t snapshot[256];

    flash_mock_reset();
    ctr_boot(1UL);

    /* Within already-logged block 1, low voltage still allows packets 0..255 */
    for (i = 0U; i < 256U; i++) {
        ASSERT_EQ_INT(ctr_next(2500U, &c), 0);
        ASSERT_EQ_U32(c, 0x00000100UL + i);
    }

    /* Block boundary: < 2700 mV must hold packet and leave flash untouched */
    memcpy(snapshot, g_mock_info, sizeof(snapshot));
    ASSERT_EQ_INT(ctr_next(2699U, &c), -1);
    ASSERT_TRUE(memcmp(snapshot, g_mock_info, sizeof(snapshot)) == 0);

    /* Once voltage recovers >= 2700 mV, block 2 is logged and packet proceeds */
    ASSERT_EQ_INT(ctr_next(2700U, &c), 0);
    ASSERT_EQ_U32(c, 0x00000200UL);

    /* Ceiling hold at CTR_BLOCK_MAX (0xFEFFFF): 256 packets OK, 257th held */
    flash_mock_reset();
    ctr_boot(CTR_BLOCK_MAX);
    for (i = 0U; i < 256U; i++) {
        ASSERT_EQ_INT(ctr_next(3000U, &c), 0);
        ASSERT_EQ_U32(c, (CTR_BLOCK_MAX << 8) + i);
    }
    ASSERT_EQ_INT(ctr_next(3000U, &c), -1);
}

static void test_wor_id_derivation(void) {
    uint8_t addr8 = 0U;
    uint16_t tag16 = 0U;
    uint8_t wake[6];
    char id_buf[9];
    uint16_t i;

    /* Known FNV-1a 32-bit vector for "2S5F554X": h = 0x4697FE37 */
    cc1101_wor_derive_id("2S5F554X", &addr8, &tag16);
    ASSERT_EQ_INT(addr8, 0xC9);
    ASSERT_EQ_U32(tag16, 0x4697UL);

    /* Verify 6-byte WOR wake frame packing for "2S5F554X" */
    wake[0] = addr8;
    wake[1] = (uint8_t)'W';
    wake[2] = (uint8_t)'O';
    wake[3] = (uint8_t)(tag16 >> 8);
    wake[4] = (uint8_t)(tag16 & 0xFFU);
    wake[5] = 100U;
    ASSERT_EQ_INT(wake[0], 0xC9);
    ASSERT_EQ_INT(wake[1], 0x57);
    ASSERT_EQ_INT(wake[2], 0x4F);
    ASSERT_EQ_INT(wake[3], 0x46);
    ASSERT_EQ_INT(wake[4], 0x97);
    ASSERT_EQ_INT(wake[5], 100);

    /* Hardware address must never be 0x00 (broadcast) or 0xFF (broadcast) */
    for (i = 0U; i < 512U; i++) {
        uint8_t a = 0U;
        uint16_t t = 0U;
        snprintf(id_buf, sizeof(id_buf), "SR%06u", (unsigned)i);
        cc1101_wor_derive_id(id_buf, &a, &t);
        ASSERT_TRUE(a != 0x00U && a != 0xFFU);
    }
}

static uint8_t sim_apply_dl_fast_wake(uint8_t dl10, uint8_t *fast_wake, uint8_t *ack_flush) {
    if ((dl10 & 0x80U) != 0U) {
        uint8_t new_fw = (dl10 & 0x01U) ? 1U : 0U;
        if (new_fw != *fast_wake) {
            *fast_wake = new_fw;
            *ack_flush = 1U;
            return 1U;
        }
    }
    return 0U;
}

static uint8_t sim_siren_poll_state(uint8_t armed, uint8_t sounding, uint8_t cmd_ack, uint8_t fast_wake) {
    uint8_t st = 0U;
    if (armed != 0U) {
        st |= 0x01U;
    }
    if (sounding != 0U) {
        st |= 0x02U;
    }
    if (cmd_ack != 0U) {
        st |= 0x04U;
    }
    if (fast_wake != 0U) {
        st |= 0x08U; /* SIREN_STATE_FAST_WAKE */
    }
    return st;
}

static void test_wor_downlink_and_poll_state(void) {
    static const uint8_t dev_key[16] = {
        0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
        0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C
    };
    uint8_t dl_plain[16];
    uint8_t dl_enc[16];
    uint8_t dl_dec[16];
    uint8_t fast_wake = 0U;
    uint8_t ack_flush = 0U;

    /* 0x00 legacy downlink (valid bit 0x80 clear) must leave fast_wake unchanged */
    memset(dl_plain, 0, sizeof(dl_plain));
    dl_plain[0] = 'D';
    dl_plain[1] = 'L';
    dl_plain[10] = 0x00U;
    dl_plain[14] = 'V';
    dl_plain[15] = 'S';
    aes128_ecb_encrypt(dev_key, dl_plain, dl_enc);
    aes128_ecb_decrypt(dev_key, dl_enc, dl_dec);
    ASSERT_EQ_INT(sim_apply_dl_fast_wake(dl_dec[10], &fast_wake, &ack_flush), 0);
    ASSERT_EQ_INT(fast_wake, 0);
    ASSERT_EQ_INT(ack_flush, 0);
    ASSERT_EQ_INT(sim_siren_poll_state(1U, 0U, 0U, fast_wake), 0x01);

    /* 0x81 enables fast_wake and triggers confirmation ack flush */
    dl_plain[10] = 0x81U;
    aes128_ecb_encrypt(dev_key, dl_plain, dl_enc);
    aes128_ecb_decrypt(dev_key, dl_enc, dl_dec);
    ASSERT_EQ_INT(sim_apply_dl_fast_wake(dl_dec[10], &fast_wake, &ack_flush), 1);
    ASSERT_EQ_INT(fast_wake, 1);
    ASSERT_EQ_INT(ack_flush, 1);
    ASSERT_EQ_INT(sim_siren_poll_state(0U, 0U, 0U, fast_wake), 0x08);
    ASSERT_EQ_INT(sim_siren_poll_state(1U, 1U, 1U, fast_wake), 0x0F);

    /* Repeated 0x81 and legacy 0x00 when enabled must not alter fast_wake or re-flush */
    ack_flush = 0U;
    ASSERT_EQ_INT(sim_apply_dl_fast_wake(0x81U, &fast_wake, &ack_flush), 0);
    ASSERT_EQ_INT(sim_apply_dl_fast_wake(0x00U, &fast_wake, &ack_flush), 0);
    ASSERT_EQ_INT(fast_wake, 1);
    ASSERT_EQ_INT(ack_flush, 0);

    /* 0x80 disables fast_wake and triggers confirmation ack flush */
    dl_plain[10] = 0x80U;
    aes128_ecb_encrypt(dev_key, dl_plain, dl_enc);
    aes128_ecb_decrypt(dev_key, dl_enc, dl_dec);
    ASSERT_EQ_INT(sim_apply_dl_fast_wake(dl_dec[10], &fast_wake, &ack_flush), 1);
    ASSERT_EQ_INT(fast_wake, 0);
    ASSERT_EQ_INT(ack_flush, 1);
    ASSERT_EQ_INT(sim_siren_poll_state(1U, 0U, 0U, fast_wake), 0x01);

    /* Repeated 0x80 when disabled must not re-trigger ack flush */
    ack_flush = 0U;
    ASSERT_EQ_INT(sim_apply_dl_fast_wake(0x80U, &fast_wake, &ack_flush), 0);
    ASSERT_EQ_INT(ack_flush, 0);
}

int main(void) {
    test_aes_fips197_appendix_b();
    test_aes_fips197_appendix_c1();
    test_openverisure_kdf_and_packet_roundtrip();
    test_flash_ctr_blank_and_blocks();
    test_flash_ctr_ping_pong_rollover();
    test_flash_ctr_cut_write_and_skip();
    test_flash_ctr_low_batt_and_ceiling();
    test_wor_id_derivation();
    test_wor_downlink_and_poll_state();

    if (g_failures == 0) {
        printf("All SIREN firmware host tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed.\n", g_failures);
    return 1;
}

