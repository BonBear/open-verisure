/**
 * @file fuzz_crypto.cpp
 * @brief Fuzz/property tests of crypto.cpp: cryptoProcessPacket (24 bytes) and
 *        cryptoProcessBigPacket (56 bytes) with random frames, bit-flipped frames and frames
 *        encrypted with the public CI test master key 00 01 .. 0F (so that authentication passes
 *        and the replay filter and the picture tag window are exercised). The acceptance of
 *        every authentic frame is compared with a model of the documented replay rules. Picture
 *        chunks go on to the real image receiver.
 */

#include "fuzz.h"
#include "crypto.h"
#include "downlink.h"
#include "host_mqtt.h"
#include "image_receiver.h"
#include <Arduino.h>
#include <mbedtls/aes.h>

void cryptoHostTestRestart();           // crypto.cpp, HOST_TEST only
void imageReceiverHostTestRestart();    // image_receiver.cpp, HOST_TEST only

namespace {

const char* CAM = "2K79A65A";            // the ID of the CI test vector

void testMaster(uint8_t m[16]) {
    for (int i = 0; i < 16; i++) m[i] = (uint8_t)i;     // public CI test master 00..0F
}

void aesEncrypt(const uint8_t k[16], const uint8_t in[16], uint8_t out[16]) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, k, 128);
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, in, out);
    mbedtls_aes_free(&aes);
}

void deviceKey(const char* id, uint8_t out[16]) {
    uint8_t m[16], block[16] = {0};
    testMaster(m);
    memcpy(block, id, 8);
    aesEncrypt(m, block, out);
}

/** 24-byte packet (see test_crypto.cpp makePlain() for the plaintext layout). */
std::vector<uint8_t> packet(const char* id, uint32_t ctr, uint8_t flags, uint8_t pct, uint16_t mv,
                            uint8_t p12, uint8_t p13) {
    uint8_t p[16] = {(uint8_t)(ctr >> 8), (uint8_t)ctr, flags, pct, (uint8_t)(mv >> 8), (uint8_t)mv,
                     (uint8_t)id[0], (uint8_t)id[1], (uint8_t)id[2], (uint8_t)id[3], 'V', 'S',
                     p12, p13, (uint8_t)(ctr >> 16), (uint8_t)(ctr >> 24)};
    uint8_t k[16];
    deviceKey(id, k);
    std::vector<uint8_t> out(24);
    memcpy(out.data(), id, 8);
    aesEncrypt(k, p, &out[8]);
    return out;
}

/** 56-byte packet: ID + 3 blocks CBC with a zero IV over the 48-byte plaintext. */
std::vector<uint8_t> bigPacket(const char* id, const uint8_t p[48]) {
    uint8_t k[16], prev[16] = {0}, x[16];
    deviceKey(id, k);
    std::vector<uint8_t> out(56);
    memcpy(out.data(), id, 8);
    for (int b = 0; b < 3; b++) {
        for (int i = 0; i < 16; i++) x[i] = p[16 * b + i] ^ prev[i];
        aesEncrypt(k, x, &out[8 + 16 * b]);
        memcpy(prev, &out[8 + 16 * b], 16);
    }
    return out;
}

/** Picture chunk plaintext: tag (start packet counter), type, chunk number, flags/length, data. */
void chunkPlain(uint8_t p[48], uint32_t tag, uint8_t type, uint16_t idx, uint8_t flagsLen, const uint8_t* data,
                size_t n) {
    memset(p, 0, 48);
    p[0] = (uint8_t)(tag >> 8);
    p[1] = (uint8_t)tag;
    p[2] = type;
    p[3] = (uint8_t)(idx >> 8);
    p[4] = (uint8_t)idx;
    p[5] = flagsLen;
    memcpy(&p[6], data, n < 40 ? n : 40);
    p[46] = (uint8_t)(tag >> 24);
    p[47] = (uint8_t)(tag >> 16);
}

bool rx24(const std::vector<uint8_t>& pkt, VerisureTelemetry* t) {
    ExactBuf b(pkt);
    uint8_t m[16];
    testMaster(m);
    memset(t, 0x5A, sizeof(*t));
    return cryptoProcessPacket(b.p, -70, 40, m, t);
}

bool rx56(const std::vector<uint8_t>& pkt) {
    ExactBuf b(pkt);
    uint8_t m[16];
    testMaster(m);
    return cryptoProcessBigPacket(b.p, m);
}

void randomId(Rng& r, char id[9]) {
    static const char an[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz";
    for (int i = 0; i < 8; i++) id[i] = an[r.below(sizeof(an) - 1)];
    id[8] = 0;
}

/** Flip 1..3 distinct bits from byte 'from' on (so the frame really changes). */
void flipBits(Rng& r, std::vector<uint8_t>& v, unsigned from) {
    unsigned n = r.range(1, 3), done[3];
    for (unsigned k = 0; k < n; k++) {
        unsigned bit;
        bool again;
        do {
            bit = r.range(from * 8, (unsigned)v.size() * 8 - 1);
            again = false;
            for (unsigned i = 0; i < k; i++) again = again || done[i] == bit;
        } while (again);
        done[k] = bit;
        v[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    }
}

/** Replay state of one device as crypto.h documents it. */
struct Model {
    char id[9];
    bool hasLast = false;
    bool ctrMode = false;
    uint32_t lastCtr = 0;
    uint16_t lastSeq = 0;

    bool accepts(uint32_t ctr, uint8_t flags) const {
        uint16_t seq = (uint16_t)ctr;
        if (flags & 0x20) return !(hasLast && ctrMode && ctr <= lastCtr);
        if (ctrMode) return false;
        if (hasLast && !(flags & 0x08)) {
            int32_t diff = (int32_t)seq - (int32_t)lastSeq;
            if (diff <= 0 && diff > -32768) return false;
        }
        return true;
    }
    void take(uint32_t ctr, uint8_t flags) {
        if (flags & 0x20) {
            ctrMode = true;
            lastCtr = ctr;
        }
        lastSeq = (uint16_t)ctr;
        hasLast = true;
    }
};

}  // namespace

/* ---- Random frames never authenticate ------------------------------------------------------- */
TEST(fuzz_crypto_random_frames) {
    Rng r(0xC2970001);
    VerisureTelemetry t;
    REQUIRE(rx24(packet(CAM, 0x1000, 0x20, 90, 3000, 0, 0), &t));     // a camera with a floor
    const int n = fuzzIters(40000);
    for (int it = 0; it < n; it++) {
        std::vector<uint8_t> f24(24), f56(56);
        r.fill(f24.data(), 24);
        r.fill(f56.data(), 56);
        char id[9];
        unsigned kind = r.below(3);
        if (kind == 0) memcpy(id, CAM, 9);
        else randomId(r, id);
        if (kind < 2 || r.chance(50)) {
            memcpy(f24.data(), id, 8);
            memcpy(f56.data(), id, 8);
        }
        CHECK(!rx24(f24, &t));
        CHECK(!rx24(std::vector<uint8_t>(f56.begin(), f56.begin() + 24), &t));
        rx56(f56);                              // about 1 in 200 is "handled" (dropped as stale)
        if (fuzzStop("fuzz_crypto_random_frames", it)) return;
    }
    // random 56-byte frames create no sessions: 31 more devices still fit next to the camera
    char id[9];
    for (int i = 0; i < 31; i++) {
        snprintf(id, sizeof(id), "N%07u", (unsigned)i);
        CHECK(rx24(packet(id, 0x0100, 0x20, 90, 3000, 0, 0), &t));
    }
    CHECK(!rx24(packet("N9999999", 0x0100, 0x20, 90, 3000, 0, 0), &t));
}

/* ---- Authentic, replayed and bit-flipped 24-byte packets against the replay model ------------ */
TEST(fuzz_crypto_replay_matches_model) {
    Rng r(0xC2970002);
    std::vector<Model> dev(6);
    for (size_t i = 0; i < dev.size(); i++) snprintf(dev[i].id, sizeof(dev[i].id), "R%07u", (unsigned)i);
    std::vector<std::vector<uint8_t>> sent;     // earlier authentic packets, for replays
    VerisureTelemetry t;
    const int n = fuzzIters(40000);
    for (int it = 0; it < n; it++) {
        Model& m = dev[r.below((uint32_t)dev.size())];
        bool ctrDevice = (&m - &dev[0]) < 4;    // devices 0..3 send the persistent counter
        unsigned op = r.below(100);
        if (op < 55) {                                          // an authentic packet
            uint32_t ctr;
            switch (r.below(5)) {
            case 0: ctr = m.lastCtr + r.range(1, 3); break;
            case 1: ctr = m.lastCtr - r.below(3); break;
            case 2: ctr = (m.lastCtr & ~0xFFu) + 0x100 * r.range(1, 3); break;
            case 3: ctr = (uint32_t)r.next(); break;
            default: ctr = m.lastSeq + r.range(0, 70000) - 32768; break;
            }
            uint8_t flags = (uint8_t)(r.byte() & 0xDF);
            if (ctrDevice ? r.chance(97) : r.chance(1)) flags |= 0x20;
            uint8_t pct = r.byte(), p12 = r.byte(), p13 = r.byte();
            uint16_t mv = (uint16_t)r.next();
            std::vector<uint8_t> pkt = packet(m.id, ctr, flags, pct, mv, p12, p13);
            bool want = m.accepts(ctr, flags);
            bool got = rx24(pkt, &t);
            CHECK_EQ(got, want);
            if (got) {
                CHECK_STR(t.deviceId, m.id);
                CHECK(t.valid);
                CHECK_EQ(t.sequence, (uint16_t)ctr);
                CHECK_EQ(t.hasCounter, (flags & 0x20) != 0);
                CHECK_EQ(t.counter, (flags & 0x20) ? ctr : (uint16_t)ctr);
                CHECK_EQ(t.batteryPct, pct);
                CHECK_EQ(t.batteryMv, mv);
                CHECK_EQ(t.rawP0, p12);
                CHECK_EQ(t.rawP1, p13);
                CHECK_EQ(t.isOpen, (flags & 1) != 0);
                CHECK_EQ(t.isPowerOn, (flags & 8) != 0);
                m.take(ctr, flags);
                if (sent.size() < 512) sent.push_back(pkt);
                else sent[r.below(512)] = pkt;
            }
        } else if (op < 70) {                                   // a bit-flipped authentic packet
            std::vector<uint8_t> pkt = packet(m.id, m.lastCtr + 1, ctrDevice ? 0x20 : 0x08, 50, 3000, 1, 2);
            flipBits(r, pkt, 0);
            CHECK(!rx24(pkt, &t));
        } else if (op < 85) {                                   // a replay of an earlier packet
            if (sent.empty()) continue;
            const std::vector<uint8_t>& pkt = sent[r.below((uint32_t)sent.size())];
            // the model decides: a replayed old-format power-on packet is let through (documented)
            Model* owner = nullptr;
            for (Model& x : dev)
                if (memcmp(x.id, pkt.data(), 8) == 0) owner = &x;
            REQUIRE(owner);
            uint8_t k[16], p[16];
            deviceKey(owner->id, k);
            mbedtls_aes_context aes;
            mbedtls_aes_init(&aes);
            mbedtls_aes_setkey_dec(&aes, k, 128);
            mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, &pkt[8], p);
            mbedtls_aes_free(&aes);
            uint32_t ctr = ((uint32_t)p[15] << 24) | ((uint32_t)p[14] << 16) | ((uint32_t)p[0] << 8) | p[1];
            bool want = owner->accepts(ctr, p[2]);
            CHECK_EQ(rx24(pkt, &t), want);
            if (want) owner->take(ctr, p[2]);
            if (p[2] & 0x20) CHECK(!want);                      // a counter packet never comes back
        } else if (op < 95) {
            hostAdvanceMillis(r.below(r.chance(50) ? 3000 : 600000));
            cryptoReplayFlush();
        } else {                                                // gateway restart after a flush
            hostAdvanceMillis(2001);
            cryptoReplayFlush();
            cryptoHostTestRestart();
            for (Model& x : dev) {
                if (x.ctrMode) x.lastSeq = 0;                   // the floor comes back from NVS
                else x.hasLast = false;                         // the old format starts afresh
            }
        }
        if (fuzzStop("fuzz_crypto_replay_matches_model", it)) return;
    }
}

/* ---- 56-byte picture chunks: the tag window against a model ---------------------------------- */
// The model follows crypto.h: the tag is rebuilt from its low 16 bits next to the last counter and
// must lie within PIC_TAG_BEHIND (128) before or PIC_TAG_AHEAD (64) after it; a new chunk (0x82)
// must belong to the newest picture taken or a newer one (any, if none was taken since the session
// began), a resent chunk (0x83) to exactly that picture. Run from several counter start values,
// also above 2^31 and near the 16-bit wrap, with gateway restarts and 'replay reset' in between.
TEST(fuzz_crypto_picture_tag_window) {
    Rng r(0xC2970003);
    static const uint32_t starts[] = {0x00000010, 0x00001000, 0x0000FFF0, 0x7FFFFFC0, 0x80000010, 0xFFFF0000};
    const unsigned NSTART = sizeof(starts) / sizeof(starts[0]);
    const int n = fuzzIters(60000);
    VerisureTelemetry t;
    uint32_t lastCtr = 0, picTag = 0;
    bool hasPicTag = false;
    uint8_t data[40];
    for (int it = 0; it < n; it++) {
        if (it % (n / (int)NSTART + 1) == 0) {                  // next start value, fresh world
            testResetWorld();
            imageReceiverHostTestRestart();
            downlinkInit();
            hostMqttReset();
            lastCtr = starts[(it / (n / (int)NSTART + 1)) % NSTART];
            REQUIRE(rx24(packet(CAM, lastCtr, 0x20, 90, 3000, 0xBA, 0), &t));
            hasPicTag = false;
        }
        unsigned op = r.below(100);
        r.fill(data, sizeof(data));
        if (op < 15) {                                          // a poll moves the counter
            uint32_t c = lastCtr + r.range(1, 3);
            if (c < lastCtr) continue;                          // keep clear of the 32-bit end
            REQUIRE(rx24(packet(CAM, c, 0x20, 90, 3000, 0xBA, 0), &t));
            lastCtr = c;
        } else if (op < 85) {                                   // a picture chunk
            uint32_t tag;
            if (hasPicTag && r.chance(40))      // the newest picture or the next ones
                tag = picTag + r.below(3);
            else if (r.chance(85))              // around the window
                tag = lastCtr + (uint32_t)((int32_t)r.range(0, 300) - 200);
            else
                tag = lastCtr + (uint32_t)(int32_t)(int16_t)r.next();
            uint8_t type = r.chance(80) ? 0x82 : r.chance(80) ? 0x83 : r.byte();
            bool observable = r.chance(60);     // a whole blob in one chunk: published if taken
            uint8_t fl;
            uint16_t idx;
            if (observable) {
                type = 0x82;
                idx = 0;
                fl = (uint8_t)(0xC0 | r.range(4, 40));
                data[0] = 0xFF;                 // not a delta blob
            } else {
                fl = r.byte();
                idx = r.chance(50) ? (uint16_t)r.below(64) : (uint16_t)r.next();
            }
            uint8_t p[48];
            chunkPlain(p, tag, type, idx, fl, data, 40);
            bool badHigh = r.chance(10);
            if (badHigh) p[46 + r.below(2)] ^= (uint8_t)(1u << r.below(8));
            unsigned len = fl & 0x3F;
            bool wellFormed = (type == 0x82 || type == 0x83) && len >= 1 && len <= 40;
            int16_t dd = (int16_t)(uint16_t)((uint16_t)tag - (uint16_t)lastCtr);
            uint32_t rebuilt = lastCtr + (uint32_t)(int32_t)dd;
            bool inWindow = !badHigh && rebuilt == tag && dd <= 64 && dd >= -128;
            bool taken = wellFormed && inWindow &&
                         (type == 0x83 ? hasPicTag && tag == picTag : !hasPicTag || (int32_t)(tag - picTag) >= 0);
            imageReceiverPublishPending();      // a finished picture is published after the poll answer
            int before = hostMqtt.images;
            CHECK_EQ(rx56(bigPacket(CAM, p)), wellFormed);
            if (taken) {
                picTag = tag;
                hasPicTag = true;
            }
            imageReceiverPublishPending();
            if (observable && wellFormed) CHECK_EQ(hostMqtt.images > before, taken);
        } else if (op < 92) {                                   // a bit-flipped chunk
            uint8_t p[48];
            chunkPlain(p, lastCtr, 0x82, (uint16_t)r.below(2000), (uint8_t)(0x80 | r.range(1, 40)), data, 40);
            std::vector<uint8_t> pkt = bigPacket(CAM, p);
            flipBits(r, pkt, 0);
            rx56(pkt);                          // CBC without a MAC: may pass, must not crash
            // whether it was taken is not modelled: resynchronise with an observable newer picture
            uint8_t q[48];
            chunkPlain(q, lastCtr + 1, 0x82, 0, 0xC0 | 8, data, 40);
            q[6] = 0xFF;
            imageReceiverPublishPending();
            int before = hostMqtt.images;
            CHECK(rx56(bigPacket(CAM, q)));
            imageReceiverPublishPending();
            if (hostMqtt.images > before) {
                picTag = lastCtr + 1;
                hasPicTag = true;
            }
        } else if (op < 97) {
            hostAdvanceMillis(r.below(30000));
            imageReceiverLoop();
            imageReceiverPollSeen(CAM);
        } else if (op < 99) {                                   // gateway restart
            hostAdvanceMillis(2001);
            cryptoReplayFlush();
            cryptoHostTestRestart();
            hasPicTag = false;
        } else {                                                // camera reflashed: replay reset, boot
            cryptoReplayReset(CAM);
            uint8_t p[48];
            chunkPlain(p, 0x0101, 0x82, 0, 0xC0 | 8, data, 40);
            CHECK(!rx56(bigPacket(CAM, p)));                    // ignored until the boot packet
            lastCtr = 0x100 * r.range(1, 8);
            REQUIRE(rx24(packet(CAM, lastCtr, 0x20, 90, 3000, 0xBA, 0), &t));
            hasPicTag = false;
        }
        if (fuzzStop("fuzz_crypto_picture_tag_window", it)) return;
    }
}

/* ---- Whole pictures through the radio layer, then replayed ---------------------------------- */
TEST(fuzz_crypto_pictures_end_to_end) {
    Rng r(0xC2970004);
    imageReceiverHostTestRestart();
    downlinkInit();
    hostMqttReset();
    VerisureTelemetry t;
    uint32_t ctr = 0x00020000 + r.below(0x10000);
    REQUIRE(rx24(packet(CAM, ctr, 0x20, 90, 3000, 0xBA, 0), &t));
    std::vector<std::vector<uint8_t>> oldFrames;
    const int n = fuzzIters(60);
    for (int it = 0; it < n; it++) {
        ctr += r.range(1, 4);                                   // the picture's start packet
        REQUIRE(rx24(packet(CAM, ctr, 0x20, 90, 3000, 0, 0), &t));
        uint32_t tag = ctr;
        FuzzJpeg j = fuzzMakeJpeg(r, 320, (int)r.below(256), r.range(5, 200), r.range(1, 40));
        std::vector<uint8_t> pic = fuzzAssemble(j);
        unsigned nc = (unsigned)((pic.size() + 39) / 40);
        std::vector<std::vector<uint8_t>> frames;
        for (unsigned c = 0; c < nc; c++) {
            size_t len = pic.size() - c * 40 < 40 ? pic.size() - c * 40 : 40;
            uint8_t p[48];
            chunkPlain(p, tag, 0x82, (uint16_t)c, (uint8_t)((c == 0 ? 0x80 : 0) | (c + 1 == nc ? 0x40 : 0) | len),
                       &pic[c * 40], len);
            frames.push_back(bigPacket(CAM, p));
        }
        for (const std::vector<uint8_t>& f : frames) {
            if (r.chance(10)) {                                 // noise: a damaged copy first
                std::vector<uint8_t> bad = f;
                flipBits(r, bad, 8 + 32);                       // last block: the tag high half
                rx56(bad);
            }
            CHECK(rx56(f));
        }
        size_t len = 0;
        const uint8_t* got = imageReceiverJpeg(&len, nullptr);
        CHECK(got && std::vector<uint8_t>(got, got + len) == pic);
        imageReceiverPublishPending();          // the poll answer after the last chunk
        CHECK(hostMqtt.last == pic);
        // a recorded older picture is dropped once the counter has moved on
        if (!oldFrames.empty()) {
            int before = hostMqtt.images;
            for (const std::vector<uint8_t>& f : oldFrames) CHECK(rx56(f));
            imageReceiverPublishPending();
            CHECK_EQ(hostMqtt.images, before);
        }
        oldFrames = frames;
        for (int k = 0; k < 2; k++) {                           // polls after the picture
            ctr += 1;
            REQUIRE(rx24(packet(CAM, ctr, 0x20, 90, 3000, 0xBA, 0), &t));
        }
        if (fuzzStop("fuzz_crypto_pictures_end_to_end", it)) return;
    }
}
