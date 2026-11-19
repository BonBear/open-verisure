/**
 * @file test_crypto.cpp
 * @brief Host tests of crypto.cpp: key derivation, packet authentication, the replay filter
 *        (persistent counter, NVS floor, 'replay reset') and the picture chunk path.
 *
 * Only the public CI test master key 00 01 .. 0F is used (see .github/scripts/check_secrets.py).
 * Packets are built here with mbedTLS directly, not with the code under test.
 */

#include "test.h"
#include "crypto.h"
#include "image_receiver.h"
#include <Preferences.h>
#include <mbedtls/aes.h>

void cryptoHostTestRestart();       // crypto.cpp, HOST_TEST only

static const char* CAM = "2K79A65A";    // the ID of the CI test vector

/* ---- Image receiver stand-in: counts what the crypto layer hands over ----------------------- */
static int s_chunks = 0, s_resends = 0;
static uint16_t s_lastIdx = 0;
static bool s_lastFirst = false, s_lastLast = false;
static uint8_t s_lastLen = 0, s_lastData0 = 0;

void imageReceiverProcessChunk(const char*, uint16_t idx, uint8_t len, bool first, bool last, const uint8_t* data) {
    s_chunks++;
    s_lastIdx = idx;
    s_lastLen = len;
    s_lastFirst = first;
    s_lastLast = last;
    s_lastData0 = data[0];
}

void imageReceiverProcessResend(const char*, uint16_t idx, uint8_t len, bool last, const uint8_t* data) {
    s_resends++;
    s_lastIdx = idx;
    s_lastLen = len;
    s_lastLast = last;
    s_lastData0 = data[0];
}

/* ---- Helpers --------------------------------------------------------------------------------- */
static void testMaster(uint8_t m[16]) {
    for (int i = 0; i < 16; i++) m[i] = (uint8_t)i;     // public CI test master 00..0F
}

static void aesEncrypt(const uint8_t k[16], const uint8_t in[16], uint8_t out[16]) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, k, 128);
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, in, out);
    mbedtls_aes_free(&aes);
}

/** Reference KDF: AES-128-ECB(master, ID padded with zeros to 16 bytes). */
static void refDeviceKey(const char* id, uint8_t out[16]) {
    uint8_t m[16], block[16] = {0};
    testMaster(m);
    memcpy(block, id, strlen(id) < 8 ? strlen(id) : 8);
    aesEncrypt(m, block, out);
}

static void toHex(const uint8_t* in, int n, char* out) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < n; i++) {
        out[2 * i] = hex[in[i] >> 4];
        out[2 * i + 1] = hex[in[i] & 15];
    }
    out[2 * n] = 0;
}

/** Plaintext of a 24-byte packet: [0..1] counter low 16 bits, [2] flags, [3] battery %, [4..5] mV,
 *  [6..9] ID[0..3], [10..11] 'V' 'S', [12..13] port bytes, [14] counter bits 16..23, [15] bits 24..31. */
static void makePlain(uint8_t p[16], const char* id, uint32_t ctr, uint8_t flags) {
    p[0] = (uint8_t)(ctr >> 8);
    p[1] = (uint8_t)ctr;
    p[2] = flags;
    p[3] = 87;
    p[4] = 0x0B;            // 3010 mV
    p[5] = 0xC2;
    memcpy(&p[6], id, 4);
    p[10] = 'V';
    p[11] = 'S';
    p[12] = 0x12;
    p[13] = 0x34;
    p[14] = (uint8_t)(ctr >> 16);
    p[15] = (uint8_t)(ctr >> 24);
}

/** 8-byte plaintext ID followed by the plaintext encrypted with the device key of that ID. */
static void seal(uint8_t out[24], const char* id, const uint8_t p[16]) {
    uint8_t k[16];
    refDeviceKey(id, k);
    memcpy(out, id, 8);
    aesEncrypt(k, p, &out[8]);
}

static bool rx(const uint8_t pkt[24], VerisureTelemetry* t = nullptr) {
    VerisureTelemetry tmp;
    memset(&tmp, 0, sizeof(tmp));
    uint8_t m[16];
    testMaster(m);
    return cryptoProcessPacket(pkt, -71, 42, m, t ? t : &tmp);
}

/** A packet with the persistent counter (flag 0x20). */
static bool sendCtr(const char* id, uint32_t ctr, uint8_t flags = 0x20, VerisureTelemetry* t = nullptr) {
    uint8_t p[16], pkt[24];
    makePlain(p, id, ctr, flags | 0x20);
    seal(pkt, id, p);
    return rx(pkt, t);
}

/** An old-format packet: 16-bit sequence that restarts at 0 (flag 0x08 = power-on reset). */
static bool sendOld(const char* id, uint16_t seq, uint8_t flags = 0) {
    uint8_t p[16], pkt[24];
    makePlain(p, id, seq, flags & ~0x20);
    seal(pkt, id, p);
    return rx(pkt);
}

static bool nvsULong(const char* key, uint32_t* v) {
    auto& ns = hostNvs()["replay"];
    auto it = ns.find(key);
    if (it == ns.end() || it->second.type != 'L') return false;
    memcpy(v, it->second.data.data(), 4);
    return true;
}

/** 48-byte plaintext of a picture chunk (type 0x82, or 0x83 for a resent one). */
static void makeChunk(uint8_t p[48], uint32_t tag, uint8_t type, uint16_t idx, uint8_t flagsLen) {
    memset(p, 0, 48);
    p[0] = (uint8_t)(tag >> 8);
    p[1] = (uint8_t)tag;
    p[2] = type;
    p[3] = (uint8_t)(idx >> 8);
    p[4] = (uint8_t)idx;
    p[5] = flagsLen;
    for (int i = 0; i < 40; i++) p[6 + i] = (uint8_t)(0xA0 + i);
    p[46] = (uint8_t)(tag >> 24);
    p[47] = (uint8_t)(tag >> 16);
}

/** 56-byte packet: ID + 3 blocks CBC with a zero IV. */
static void sealBig(uint8_t out[56], const char* id, const uint8_t p[48]) {
    uint8_t k[16], prev[16] = {0}, x[16];
    refDeviceKey(id, k);
    memcpy(out, id, 8);
    for (int b = 0; b < 3; b++) {
        for (int i = 0; i < 16; i++) x[i] = p[16 * b + i] ^ prev[i];
        aesEncrypt(k, x, &out[8 + 16 * b]);
        memcpy(prev, &out[8 + 16 * b], 16);
    }
}

static bool sendChunk(const char* id, uint32_t tag, uint8_t type = 0x82, uint16_t idx = 0,
                      uint8_t flagsLen = 0x80 | 40) {
    uint8_t p[48], pkt[56], m[16];
    makeChunk(p, tag, type, idx, flagsLen);
    sealBig(pkt, id, p);
    testMaster(m);
    return cryptoProcessBigPacket(pkt, m);
}

/** Device ID: the prefix and n as 7 digits, e.g. D0000005. */
static void makeId(char id[9], char prefix, int n) {
    id[0] = prefix;
    for (int i = 7; i >= 1; i--, n /= 10) id[i] = (char)('0' + n % 10);
    id[8] = 0;
}

static void resetImageCounters() {
    s_chunks = s_resends = 0;
    s_lastIdx = 0;
    s_lastLen = s_lastData0 = 0;
    s_lastFirst = s_lastLast = false;
}

/* ---- Key derivation -------------------------------------------------------------------------- */
TEST(kdf_ci_test_vector) {
    uint8_t m[16], dk[16];
    char hex[33], kcv[7];
    testMaster(m);
    REQUIRE(cryptoDeriveDeviceKey(m, CAM, dk));
    toHex(dk, 16, hex);
    CHECK_STR(hex, "7960FF23E9DFE83C57BF94B437CCC0B9");
    cryptoKeyCheckValue(m, kcv);
    CHECK_STR(kcv, "C6A13B");
}

TEST(kdf_matches_reference_and_truncates_long_ids) {
    uint8_t m[16], a[16], b[16], ref[16];
    testMaster(m);
    REQUIRE(cryptoDeriveDeviceKey(m, "ABCDEFGH", a));
    refDeviceKey("ABCDEFGH", ref);
    CHECK(memcmp(a, ref, 16) == 0);
    REQUIRE(cryptoDeriveDeviceKey(m, "ABCDEFGHXYZ", b));    // only 8 characters are used
    CHECK(memcmp(a, b, 16) == 0);
    REQUIRE(cryptoDeriveDeviceKey(m, "ABCDEFGI", b));
    CHECK(memcmp(a, b, 16) != 0);
}

TEST(kdf_null_arguments) {
    uint8_t m[16], dk[16];
    char kcv[7];
    testMaster(m);
    CHECK(!cryptoDeriveDeviceKey(nullptr, CAM, dk));
    CHECK(!cryptoDeriveDeviceKey(m, nullptr, dk));
    CHECK(!cryptoDeriveDeviceKey(m, CAM, nullptr));
    cryptoKeyCheckValue(nullptr, kcv);
    CHECK_STR(kcv, "------");
}

TEST(encrypt_decrypt_round_trip) {
    uint8_t k[16], in[16], c[16], out[16];
    refDeviceKey(CAM, k);
    for (int i = 0; i < 16; i++) in[i] = (uint8_t)(i * 7 + 1);
    REQUIRE(cryptoEncryptPayload(k, in, c));
    CHECK(memcmp(c, in, 16) != 0);
    REQUIRE(cryptoDecryptPayload(k, c, out));
    CHECK(memcmp(out, in, 16) == 0);
}

/* ---- Packet authentication ------------------------------------------------------------------- */
TEST(packet_valid_fields) {
    VerisureTelemetry t;
    memset(&t, 0, sizeof(t));
    // flags: open + low battery + counter, reset cause 2 in bits 7:6
    REQUIRE(sendCtr(CAM, 0x00030102, 0x01 | 0x10 | 0x80, &t));
    CHECK(t.valid);
    CHECK_STR(t.deviceId, CAM);
    CHECK(t.hasCounter);
    CHECK_EQ(t.counter, 0x00030102);
    CHECK_EQ(t.sequence, 0x0102);
    CHECK(t.isOpen);
    CHECK(!t.isShock);
    CHECK(!t.isButton);
    CHECK(!t.isPowerOn);
    CHECK(t.isLowBatt);
    CHECK_EQ(t.batteryPct, 87);
    CHECK_EQ(t.batteryMv, 3010);
    CHECK_EQ(t.rawP0, 0x12);
    CHECK_EQ(t.rawP1, 0x34);
    CHECK_EQ(t.rawP2, 0);
    CHECK_EQ(t.rawRst, 2);
    CHECK_EQ(t.rssi, -71);
    CHECK_EQ(t.lqi, 42);
}

TEST(packet_rejects_wrong_id_echo) {
    uint8_t p[16], pkt[24];
    makePlain(p, CAM, 0x100, 0x20);
    p[9] ^= 0x01;                       // echo of ID[3] does not match
    seal(pkt, CAM, p);
    CHECK(!rx(pkt));
    makePlain(p, CAM, 0x100, 0x20);
    memcpy(&p[6], "2K7X", 4);
    seal(pkt, CAM, p);
    CHECK(!rx(pkt));
}

TEST(packet_rejects_missing_vs) {
    uint8_t p[16], pkt[24];
    makePlain(p, CAM, 0x100, 0x20);
    p[10] = 'v';
    seal(pkt, CAM, p);
    CHECK(!rx(pkt));
    makePlain(p, CAM, 0x100, 0x20);
    p[11] = 0;
    seal(pkt, CAM, p);
    CHECK(!rx(pkt));
}

TEST(packet_rejects_other_device_header_and_corruption) {
    uint8_t p[16], pkt[24];
    makePlain(p, CAM, 0x100, 0x20);
    seal(pkt, CAM, p);
    uint8_t moved[24];
    memcpy(moved, pkt, 24);
    memcpy(moved, "2K79A65B", 8);       // ciphertext of one device under another device's ID
    CHECK(!rx(moved));
    memcpy(moved, pkt, 24);
    moved[15] ^= 0x40;                  // one flipped ciphertext bit
    CHECK(!rx(moved));
    CHECK(rx(pkt));                     // the untouched packet is fine
}

TEST(packet_rejects_non_alphanumeric_id) {
    uint8_t p[16], pkt[24];
    makePlain(p, "2K79A65-", 0x100, 0x20);
    seal(pkt, "2K79A65-", p);
    CHECK(!rx(pkt));
    makePlain(p, "2K79A65\"", 0x100, 0x20);
    seal(pkt, "2K79A65\"", p);
    CHECK(!rx(pkt));
}

TEST(packet_null_arguments) {
    uint8_t p[16], pkt[24], m[16];
    VerisureTelemetry t;
    testMaster(m);
    makePlain(p, CAM, 0x100, 0x20);
    seal(pkt, CAM, p);
    CHECK(!cryptoProcessPacket(nullptr, 0, 0, m, &t));
    CHECK(!cryptoProcessPacket(pkt, 0, 0, nullptr, &t));
    CHECK(!cryptoProcessPacket(pkt, 0, 0, m, nullptr));
}

/* ---- Replay filter: persistent counter ------------------------------------------------------- */
TEST(replay_counter_strictly_increasing) {
    CHECK(sendCtr(CAM, 0x0200));
    CHECK(!sendCtr(CAM, 0x0200));       // equal: dropped
    CHECK(!sendCtr(CAM, 0x01FF));       // lower: dropped
    CHECK(!sendCtr(CAM, 0x0000));
    CHECK(sendCtr(CAM, 0x0201));
    CHECK(sendCtr(CAM, 0x00120000));    // the high half counts too
    CHECK(!sendCtr(CAM, 0x0202));
    CHECK(!sendCtr(CAM, 0x00120000, 0x08 | 0x04));   // no exception for power-on or tamper
}

TEST(replay_counter_device_never_falls_back_to_old_format) {
    CHECK(sendCtr(CAM, 0x0300));
    CHECK(!sendOld(CAM, 0x0400));
    CHECK(!sendOld(CAM, 0x0000, 0x08));  // not even with the power-on flag
}

TEST(replay_floor_survives_restart) {
    uint32_t v = 0;
    CHECK(!cryptoReplayFloor(CAM, &v));
    CHECK(sendCtr(CAM, 0x0500));
    CHECK(cryptoReplayFloor(CAM, &v));
    CHECK_EQ(v, 0x0500);
    CHECK(nvsULong(CAM, &v));           // the first packet is written at once
    CHECK_EQ(v, 0x0500);

    cryptoHostTestRestart();            // gateway restart: RAM gone, NVS kept
    CHECK(cryptoReplayFloor(CAM, &v));
    CHECK_EQ(v, 0x0500);
    CHECK(!sendCtr(CAM, 0x0500));
    CHECK(!sendCtr(CAM, 0x04FF));
    CHECK(!sendOld(CAM, 0x0600, 0x08));
    CHECK(sendCtr(CAM, 0x0501));
}

TEST(replay_nvs_write_is_rate_limited_and_flushed) {
    uint32_t v = 0;
    CHECK(sendCtr(CAM, 0x0200));                // t = 100000: written
    hostAdvanceMillis(500);
    CHECK(sendCtr(CAM, 0x0201));                // same block within 2 s: pending
    CHECK(nvsULong(CAM, &v));
    CHECK_EQ(v, 0x0200);
    cryptoReplayFlush();                        // only 0.5 s since the write
    CHECK(nvsULong(CAM, &v));
    CHECK_EQ(v, 0x0200);
    hostAdvanceMillis(1499);
    cryptoReplayFlush();                        // 1.999 s
    CHECK(nvsULong(CAM, &v));
    CHECK_EQ(v, 0x0200);
    hostAdvanceMillis(1);
    cryptoReplayFlush();                        // 2 s: written
    CHECK(nvsULong(CAM, &v));
    CHECK_EQ(v, 0x0201);

    cryptoHostTestRestart();
    CHECK(!sendCtr(CAM, 0x0201));
    CHECK(sendCtr(CAM, 0x0202));
}

TEST(replay_new_block_is_written_at_once) {
    uint32_t v = 0;
    CHECK(sendCtr(CAM, 0x0200));
    hostAdvanceMillis(100);
    CHECK(sendCtr(CAM, 0x0300));                // another block (a device boot): no 2 s wait
    CHECK(nvsULong(CAM, &v));
    CHECK_EQ(v, 0x0300);
}

TEST(replay_unflushed_packet_is_the_documented_window) {
    // Documented limit: a packet accepted < 2 s before a restart and not flushed can come back once.
    CHECK(sendCtr(CAM, 0x0200));
    hostAdvanceMillis(100);
    CHECK(sendCtr(CAM, 0x0201));
    cryptoHostTestRestart();                    // no cryptoReplayFlush() before the restart
    CHECK(!sendCtr(CAM, 0x0200));
    CHECK(sendCtr(CAM, 0x0201));
}

/* ---- Replay filter: old 16-bit format -------------------------------------------------------- */
TEST(replay_old_format_sequence) {
    const char* id = "OLDDOOR1";
    CHECK(sendOld(id, 5));
    CHECK(!sendOld(id, 5));
    CHECK(!sendOld(id, 4));
    CHECK(sendOld(id, 6));
    CHECK(!sendOld(id, 3, 0x04));               // tamper gets no exception
    CHECK(sendOld(id, 0, 0x08));                // power-on reset restarts the sequence
    CHECK(sendOld(id, 1));
    CHECK(sendOld(id, 0xFFFF));                 // far ahead (wrap window)
    CHECK(sendOld(id, 0x0000));                 // wraps around
}

/* ---- 'replay reset' -------------------------------------------------------------------------- */
TEST(replay_reset_accepts_only_early_boot_packet) {
    uint32_t v = 0;
    char rkey[10] = "R";
    strcat(rkey, CAM);
    CHECK(sendCtr(CAM, 0x1234));
    CHECK(cryptoReplayReset(CAM));              // had replay state
    CHECK(!nvsULong(CAM, &v));                  // floor removed
    CHECK(nvsULong(rkey, &v));                  // waiting state in NVS
    CHECK_EQ(v, 8u << 8);

    CHECK(!sendCtr(CAM, 0x1235));               // a recorded packet above the old floor
    CHECK(!sendCtr(CAM, 0x1300));               // a recorded boot packet of a high block
    CHECK(!sendCtr(CAM, 0x0900));               // block 9 > default limit 8
    CHECK(!sendCtr(CAM, 0x0301));               // not a boot packet (low byte != 0)
    CHECK(!sendOld(CAM, 0x0000, 0x08));         // no old-format packet either
    CHECK(sendCtr(CAM, 0x0300));                // boot packet of block 3
    CHECK(!nvsULong(rkey, &v));                 // waiting state removed
    CHECK(nvsULong(CAM, &v));
    CHECK_EQ(v, 0x0300);
    CHECK(sendCtr(CAM, 0x0301));                // back to normal
    CHECK(!sendCtr(CAM, 0x0300));
}

TEST(replay_reset_block_limit_edge_and_parameter) {
    CHECK(sendCtr(CAM, 0x5000));
    cryptoReplayReset(CAM);
    CHECK(!sendCtr(CAM, 0x0801));
    CHECK(sendCtr(CAM, 0x0800));                // block 8 is the last one taken

    cryptoReplayReset(CAM, 0x20);
    CHECK(!sendCtr(CAM, 0x2100));
    CHECK(sendCtr(CAM, 0x2000));
}

TEST(replay_reset_state_survives_restart) {
    CHECK(sendCtr(CAM, 0x1234));
    cryptoReplayReset(CAM);
    cryptoHostTestRestart();
    CHECK(!sendCtr(CAM, 0x1235));
    CHECK(!sendCtr(CAM, 0x0900));
    CHECK(!sendCtr(CAM, 0x0501));
    CHECK(!sendOld(CAM, 0x0000, 0x08));
    CHECK(sendCtr(CAM, 0x0500));
    cryptoHostTestRestart();
    CHECK(!sendCtr(CAM, 0x0500));
    CHECK(sendCtr(CAM, 0x0501));
}

TEST(replay_reset_unknown_and_bad_ids) {
    CHECK(!cryptoReplayReset("NEWDEV01"));      // nothing known: false, but it waits for a boot packet
    CHECK(!sendCtr("NEWDEV01", 0x1201));
    CHECK(sendCtr("NEWDEV01", 0x0100));
    CHECK(!cryptoReplayReset("SHORT"));
    CHECK(!cryptoReplayReset("TOOLONG01"));
}

/* ---- Session table ---------------------------------------------------------------------------- */
TEST(session_table_holds_32_devices) {
    char id[9];
    for (int i = 0; i < 32; i++) {
        makeId(id, 'D', i);
        CHECK(sendCtr(id, 0x0100));
    }
    CHECK(!sendCtr("D9999999", 0x0100));        // table full: dropped, no replay state for it
    CHECK(sendCtr("D0000005", 0x0101));         // known devices go on
}

/* ---- Picture chunks (56-byte packets) -------------------------------------------------------- */
TEST(big_packet_unknown_ids_create_no_sessions) {
    char id[9];
    resetImageCounters();
    for (int i = 0; i < 40; i++) {              // more made-up IDs than the table holds
        makeId(id, 'U', i);
        CHECK(!sendChunk(id, 0x0101));
    }
    CHECK_EQ(s_chunks, 0);
    for (int i = 0; i < 31; i++) {              // 32 real devices still fit, the camera too
        makeId(id, 'D', i);
        CHECK(sendCtr(id, 0x0100));
    }
    CHECK(sendCtr(CAM, 0x0100));
    CHECK(!sendCtr("D9999999", 0x0100));
}

TEST(big_packet_fresh_device_without_floor_is_ignored) {
    resetImageCounters();
    CHECK(sendOld("OLDCAM01", 7));              // old format: no counter, no floor
    CHECK(!sendChunk("OLDCAM01", 0x0008));
    CHECK_EQ(s_chunks, 0);
}

TEST(big_packet_valid_chunks_and_stale_ones) {
    resetImageCounters();
    CHECK(sendCtr(CAM, 0x1000));
    CHECK(sendChunk(CAM, 0x1001, 0x82, 0, 0x80 | 40));     // first chunk of a picture
    CHECK_EQ(s_chunks, 1);
    CHECK(s_lastFirst);
    CHECK(!s_lastLast);
    CHECK_EQ(s_lastLen, 40);
    CHECK_EQ(s_lastData0, 0xA0);
    CHECK(sendChunk(CAM, 0x1001, 0x82, 1, 0x40 | 17));     // last chunk, 17 bytes
    CHECK_EQ(s_chunks, 2);
    CHECK_EQ(s_lastIdx, 1);
    CHECK(s_lastLast);
    CHECK_EQ(s_lastLen, 17);

    CHECK(sendChunk(CAM, 0x0FFF, 0x82, 0));                // older picture: handled, dropped
    CHECK_EQ(s_chunks, 2);
    CHECK(sendChunk(CAM, 0x1001, 0x83, 1, 0x40 | 17));     // resend of the newest picture
    CHECK_EQ(s_resends, 1);
    CHECK(sendChunk(CAM, 0x0FFF, 0x83, 1, 17));            // resend of an older one: dropped
    CHECK_EQ(s_resends, 1);

    CHECK(sendChunk(CAM, 0x1000 + 65, 0x82, 0));           // too far ahead: dropped
    CHECK_EQ(s_chunks, 2);
    CHECK(sendChunk(CAM, 0x1000 + 64, 0x82, 0));           // the limit
    CHECK_EQ(s_chunks, 3);
    CHECK(sendChunk(CAM, 0x1001, 0x83, 0));                // no longer the newest picture
    CHECK_EQ(s_resends, 1);

    uint32_t v = 0;
    CHECK(cryptoReplayFloor(CAM, &v));
    CHECK_EQ(v, 0x1000);                                   // pictures never move the counter
}

TEST(big_packet_window_behind) {
    resetImageCounters();
    CHECK(sendCtr(CAM, 0x1000));
    CHECK(sendChunk(CAM, 0x1000 - 129, 0x82, 0));
    CHECK_EQ(s_chunks, 0);
    CHECK(sendChunk(CAM, 0x1000 - 128, 0x82, 0));
    CHECK_EQ(s_chunks, 1);
}

TEST(big_packet_wrong_tag_high_half) {
    uint8_t p[48], pkt[56], m[16];
    resetImageCounters();
    testMaster(m);
    CHECK(sendCtr(CAM, 0x00051000));
    makeChunk(p, 0x00051001, 0x82, 0, 0x80 | 40);
    p[47] ^= 0x01;
    sealBig(pkt, CAM, p);
    CHECK(cryptoProcessBigPacket(pkt, m));
    CHECK_EQ(s_chunks, 0);
    CHECK(sendChunk(CAM, 0x00051001));
    CHECK_EQ(s_chunks, 1);
}

TEST(big_packet_bad_type_or_length) {
    resetImageCounters();
    CHECK(sendCtr(CAM, 0x1000));
    CHECK(!sendChunk(CAM, 0x1001, 0x81, 0));
    CHECK(!sendChunk(CAM, 0x1001, 0x82, 0, 0x80 | 0));
    CHECK(!sendChunk(CAM, 0x1001, 0x82, 0, 0x80 | 41));
    CHECK_EQ(s_chunks, 0);
    uint8_t p[48], pkt[56], m[16];
    testMaster(m);
    makeChunk(p, 0x1001, 0x82, 0, 0x80 | 40);
    sealBig(pkt, CAM, p);
    pkt[3] = '-';                                          // non-alphanumeric ID
    CHECK(!cryptoProcessBigPacket(pkt, m));
    CHECK(!cryptoProcessBigPacket(nullptr, m));
    CHECK(!cryptoProcessBigPacket(pkt, nullptr));
}

TEST(big_packet_after_restart_uses_nvs_floor) {
    resetImageCounters();
    CHECK(sendCtr(CAM, 0x2000));
    cryptoHostTestRestart();
    CHECK(sendChunk(CAM, 0x2001));
    CHECK_EQ(s_chunks, 1);
}

TEST(big_packet_ignored_while_waiting_for_boot) {
    resetImageCounters();
    CHECK(sendCtr(CAM, 0x2000));
    cryptoReplayReset(CAM);
    CHECK(!sendChunk(CAM, 0x0001));
    CHECK(!sendChunk(CAM, 0x2001));
    cryptoHostTestRestart();                               // known from the "R" key only
    CHECK(!sendChunk(CAM, 0x0001));
    CHECK_EQ(s_chunks, 0);
}
