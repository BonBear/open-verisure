/**
 * @file fuzz_image_receiver.cpp
 * @brief Fuzz/property tests of image_receiver.cpp (picture chunk reassembly, delta splicing,
 *        grayscale and large pictures, resend of lost chunks). The receiver runs against the
 *        real downlink.cpp and jpeg_splice.cpp and the MQTT recorder (stubs/host_mqtt.h).
 *
 * Property tests send valid pictures and deltas and check that what is stored and published is
 * exactly the picture (or the reference splice), also after lost chunks were resent. The random
 * test drives every entry point with random and mutated input (huge chunk numbers, lengths
 * 0..255, out of order, duplicates, resends of chunks never sent, interleaved device IDs, time
 * jumps) and checks after every step that the outputs stay within their buffers.
 */

#include "fuzz.h"
#include "downlink.h"
#include "host_mqtt.h"
#include "image_receiver.h"
#include "jpeg_splice.h"
#include <Arduino.h>

void imageReceiverHostTestRestart();    // image_receiver.cpp, HOST_TEST only

namespace {

const unsigned CHUNK = 40;
const unsigned MAX_CHUNKS = (MAX_IMAGE_SIZE + CHUNK - 1) / CHUNK;
const size_t GRAY_MAX = 16384;          // image_receiver.cpp
const char* CAM = "CAMA0001";

void resetAll() {
    imageReceiverHostTestRestart();
    downlinkInit();
    hostMqttReset();
}

/** imageReceiverProcessChunk() with the data in an exactly sized heap block. */
void chunk(const char* dev, uint16_t idx, uint8_t len, bool first, bool last, const uint8_t* src) {
    ExactBuf b(len);
    if (len) memcpy(b.p, src, len);
    imageReceiverProcessChunk(dev, idx, len, first, last, b.p);
}

void resend(const char* dev, uint16_t idx, uint8_t len, bool last, const uint8_t* src) {
    ExactBuf b(len);
    if (len) memcpy(b.p, src, len);
    imageReceiverProcessResend(dev, idx, len, last, b.p);
}

unsigned chunksOf(size_t len) { return (unsigned)((len + CHUNK - 1) / CHUNK); }

uint8_t chunkLen(const std::vector<uint8_t>& blob, unsigned c) {
    size_t rest = blob.size() - (size_t)c * CHUNK;
    return (uint8_t)(rest < CHUNK ? rest : CHUNK);
}

/** Send a blob as the camera does: chunks in order, first and last flagged. */
void sendBlob(const char* dev, const std::vector<uint8_t>& blob) {
    unsigned n = chunksOf(blob.size());
    for (unsigned c = 0; c < n; c++) chunk(dev, (uint16_t)c, chunkLen(blob, c), c == 0, c + 1 == n, &blob[c * CHUNK]);
}

/** sendBlob(), then the publish that follows the camera's poll answer. Finishing a picture must not
 *  publish by itself (nothing is pending before: the callers flush every picture). */
void sendBlobPublished(const char* dev, const std::vector<uint8_t>& blob) {
    int before = hostMqtt.images;
    sendBlob(dev, blob);
    CHECK_EQ(hostMqtt.images, before);
    imageReceiverPublishPending();
}

/** Touch every byte of a returned picture (ASan checks the range) and check the bounds. */
void checkPicture(const uint8_t* p, size_t len, uint16_t missing, size_t maxLen, bool spliced) {
    if (!p) return;
    CHECK(len > 0 && len <= maxLen);
    if (len == 0 || len > maxLen) return;
    volatile uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum = sum + p[i];
    (void)sum;
    // A spliced picture keeps the missing count of its base (whose holes stay in the MCUs the
    // delta did not replace), so a delta that shrinks the picture can leave a count above its
    // own chunk number (informational only: X-Missing-Chunks of /api/camera.jpg).
    CHECK(missing <= (spliced ? MAX_CHUNKS : chunksOf(len)));
}

/** Invariants that hold after every step. */
void checkOutputs() {
    size_t len = 0;
    uint16_t missing = 0;
    const uint8_t* p = imageReceiverJpeg(&len, &missing);
    checkPicture(p, len, missing, MAX_IMAGE_SIZE, true);
    if (!p) CHECK_EQ(len, 0);
    p = imageReceiverJpegLarge(&len, &missing);
    checkPicture(p, len, missing, MAX_IMAGE_SIZE, false);
    if (!p) CHECK_EQ(len, 0);
    CHECK(imageReceiverJpeg(nullptr, nullptr) == imageReceiverJpeg(&len, nullptr));
    CHECK_EQ(hostMqtt.badArgs, 0);
    CHECK(hostMqtt.maxLen <= MAX_IMAGE_SIZE);
}

std::vector<uint8_t> current() {
    size_t len = 0;
    const uint8_t* p = imageReceiverJpeg(&len, nullptr);
    return p ? std::vector<uint8_t>(p, p + len) : std::vector<uint8_t>();
}

std::vector<uint8_t> currentLarge() {
    size_t len = 0;
    const uint8_t* p = imageReceiverJpegLarge(&len, nullptr);
    return p ? std::vector<uint8_t>(p, p + len) : std::vector<uint8_t>();
}

/** A resend request in this poll's answer must address chunks that exist. */
bool takeResendRequest(const char* dev, uint16_t* first, uint32_t* map) {
    uint8_t cmd = downlinkCommandForPoll(dev, 0, first, map);
    if (cmd != DOWNLINK_CMD_RESEND) return false;
    CHECK(*map != 0);
    CHECK(*first < MAX_CHUNKS);
    if (*map) CHECK(*first + (31u - (unsigned)__builtin_clz(*map)) < MAX_CHUNKS);
    return true;
}

/** A picture of random size that fits MAX_IMAGE_SIZE (and maxLen). */
FuzzJpeg randomPicture(Rng& r, unsigned width, int id, size_t maxLen) {
    for (;;) {
        unsigned segs = r.chance(3) ? r.range(900, JPEG_SPLICE_MAX_SEGS) : r.range(1, 300);
        FuzzJpeg j = fuzzMakeJpeg(r, width, id, segs, r.range(0, 45));
        if (fuzzAssemble(j).size() <= maxLen) return j;
    }
}

}  // namespace

/* ---- Valid pictures and deltas: stored and published exactly --------------------------------- */
TEST(fuzz_image_pictures_and_deltas_match_reference) {
    Rng r(0x1A6E0001);
    resetAll();
    FuzzJpeg base;
    int baseId = -1;
    std::vector<uint8_t> want;              // what imageReceiverJpeg() must return
    std::vector<uint8_t> wantLarge;
    const int n = fuzzIters(600);
    for (int it = 0; it < n; it++) {
        unsigned what = baseId < 0 ? 0 : r.below(10);
        hostAdvanceMillis(r.range(100, 3000));
        int published = hostMqtt.images;
        if (what <= 2) {                                     // full colour picture: the new base
            baseId = (int)r.below(256);
            base = randomPicture(r, 320, baseId, MAX_IMAGE_SIZE);
            want = fuzzAssemble(base);
            sendBlobPublished(CAM, want);
            CHECK(hostMqtt.last == want);
            CHECK_EQ(hostMqtt.images, published + 1);
        } else if (what <= 6) {                              // delta against the base
            std::vector<FuzzDeltaEntry> e = fuzzDeltaEntries(r, (unsigned)base.segs.size(), 30, 60);
            std::vector<uint8_t> d = fuzzMakeDelta(baseId, (unsigned)base.segs.size(), e);
            FuzzJpeg next = fuzzSplice(base, e);
            std::vector<uint8_t> nb = fuzzAssemble(next);
            if (d.size() > MAX_IMAGE_SIZE || nb.size() > MAX_IMAGE_SIZE) continue;
            sendBlobPublished(CAM, d);
            base = next;
            if (!e.empty()) {
                want = nb;                                   // the spliced picture is the newest
                CHECK(hostMqtt.last == want);
                CHECK_EQ(hostMqtt.images, published + 1);
            } else {
                CHECK_EQ(hostMqtt.images, published);        // nothing changed: not published
                if (want.empty() || jpegCommentId(want.data(), want.size()) >= 0) want = nb;
            }
        } else if (what == 7) {                              // delta for a picture we do not have
            std::vector<uint8_t> d =
                fuzzMakeDelta((baseId + 1 + (int)r.below(255)) & 0xFF, (unsigned)base.segs.size(),
                              fuzzDeltaEntries(r, (unsigned)base.segs.size(), 5, 20));
            sendBlobPublished(CAM, d);
            CHECK_EQ(hostMqtt.images, published);
            uint16_t f;
            uint32_t m;
            uint8_t cmd = downlinkCommandForPoll(CAM, 0, &f, &m);   // a full picture is asked for
            CHECK(cmd == 0 || cmd == 0xD1);                      // (at most once a minute)
            downlinkCommandForPoll(CAM, DOWNLINK_STATE_ACK, &f, &m);
        } else if (what == 8) {                              // grayscale flash picture, no COM id
            FuzzJpeg g = randomPicture(r, 320, -1, r.chance(80) ? GRAY_MAX : MAX_IMAGE_SIZE);
            std::vector<uint8_t> gb = fuzzAssemble(g);
            sendBlobPublished(CAM, gb);
            CHECK(hostMqtt.last == gb);
            if (gb.size() <= GRAY_MAX) want = gb;            // newest until a delta changes the picture
        } else {                                             // 640x480: kept apart
            FuzzJpeg l = randomPicture(r, 640, (int)r.below(256), MAX_IMAGE_SIZE);
            wantLarge = fuzzAssemble(l);
            sendBlobPublished(CAM, wantLarge);
            CHECK(hostMqtt.last == wantLarge);
        }
        CHECK(current() == want);
        CHECK(currentLarge() == wantLarge);
        checkOutputs();
        if (fuzzStop("fuzz_image_pictures_and_deltas_match_reference", it)) return;
    }
}

/* ---- Lost chunks are asked for again until the picture is complete --------------------------- */
TEST(fuzz_image_resend_completes_pictures) {
    Rng r(0x1A6E0002);
    const int n = fuzzIters(400);
    int completed = 0;
    for (int it = 0; it < n; it++) {
        resetAll();
        hostAdvanceMillis(1000);
        int id = (int)r.below(256);
        bool wide = r.chance(15);               // 640x480: stored as the large picture
        FuzzJpeg j = fuzzMakeJpeg(r, wide ? 640 : 320, id, r.range(10, 250), r.range(5, 45));
        std::vector<uint8_t> pic = fuzzAssemble(j);
        unsigned nc = chunksOf(pic.size());
        unsigned lossPct = r.range(2, 40);
        bool lossyResend = r.chance(40);        // resent chunks get lost too
        std::vector<bool> lost(nc);
        unsigned delivered = 0;
        for (unsigned c = 0; c < nc; c++) {
            lost[c] = r.chance(lossPct) || (c + 1 == nc && r.chance(30)) || (c == 0 && r.chance(10));
            if (!lost[c]) delivered++;
        }
        if (!delivered) continue;
        for (unsigned c = 0; c < nc; c++)
            if (!lost[c]) chunk(CAM, (uint16_t)c, chunkLen(pic, c), c == 0, c + 1 == nc, &pic[c * CHUNK]);
        checkOutputs();
        for (int round = 0; round < 80; round++) {
            hostAdvanceMillis(2000);               // the camera polls every ~2 s after a picture
            imageReceiverLoop();
            imageReceiverPollSeen(CAM);
            uint16_t first;
            uint32_t map;
            if (!takeResendRequest(CAM, &first, &map)) break;
            for (unsigned i = 0; i < 32; i++) {
                unsigned c = first + i;
                if (!(map & (1UL << i)) || c >= nc) continue;   // past the end: the camera has none
                if (lossyResend && r.chance(25)) continue;
                resend(CAM, (uint16_t)c, chunkLen(pic, c), c + 1 == nc, &pic[c * CHUNK]);
            }
            checkOutputs();
        }
        if (!lossyResend) {
            // complete again: stored like a picture without holes and published once more
            imageReceiverPublishPending();      // (the poll answer after the last chunk does this)
            CHECK(hostMqtt.last == pic);
            CHECK((wide ? currentLarge() : current()) == pic);
            completed++;
        }
        if (fuzzStop("fuzz_image_resend_completes_pictures", it)) return;
    }
    CHECK(completed > 0);
}

/* ---- Everything at random -------------------------------------------------------------------- */
TEST(fuzz_image_random_operations) {
    Rng r(0x1A6E0003);
    resetAll();
    static const char* devs[] = {"CAMA0001", "CAMB0002", "CAMA0001", "X", "", "CAMC0003LONGERID"};
    const unsigned NDEV = sizeof(devs) / sizeof(devs[0]);
    std::vector<uint8_t> lastBlob;
    int baseId = 0;
    unsigned segs = 1;
    uint8_t data[256];
    uint16_t seqIdx = 0;
    const int n = fuzzIters(60000);
    for (int it = 0; it < n; it++) {
        const char* dev = devs[r.below(NDEV)];
        unsigned op = r.below(100);
        r.fill(data, sizeof(data));
        uint16_t idx;
        switch (r.below(5)) {
        case 0: idx = (uint16_t)r.below(300); break;
        case 1: idx = (uint16_t)r.range(MAX_CHUNKS - 10, MAX_CHUNKS + 10); break;
        case 2: idx = (uint16_t)r.next(); break;
        default: idx = seqIdx++; if (seqIdx > MAX_CHUNKS + 2) seqIdx = 0; break;
        }
        uint8_t len = r.chance(70) ? (uint8_t)r.range(1, CHUNK) : r.chance(50) ? (uint8_t)CHUNK : r.byte();
        if (op < 35) {                                      // a random chunk
            chunk(dev, idx, len, r.chance(10), r.chance(10), data);
        } else if (op < 50) {                               // a random resent chunk
            resend(dev, idx, len, r.chance(10), data);
        } else if (op < 60) {                               // a poll: answer a resend request
            imageReceiverPollSeen(dev);
            uint16_t first;
            uint32_t map;
            if (takeResendRequest(dev, &first, &map)) {
                for (unsigned i = 0; i < 32; i++) {
                    if (!(map & (1UL << i)) || r.chance(20)) continue;
                    unsigned c = first + i;
                    if (!lastBlob.empty() && c < chunksOf(lastBlob.size()) && r.chance(70))
                        resend(dev, (uint16_t)c, chunkLen(lastBlob, c), c + 1 == chunksOf(lastBlob.size()),
                               &lastBlob[c * CHUNK]);
                    else
                        resend(dev, (uint16_t)c, (uint8_t)r.range(1, CHUNK), r.chance(5), data);
                }
            }
            if (r.chance(80)) imageReceiverAfterPollAnswer(dev);   // the answer went out (or not yet)
        } else if (op < 65) {
            imageReceiverLoop();
        } else if (op < 75) {                               // time goes by
            static const uint32_t steps[] = {100, 6000, 20000, 70000};
            hostAdvanceMillis(r.below(steps[r.below(4)]));
        } else if (op < 85) {                               // a valid blob, with losses and repeats
            unsigned kind = r.below(4);
            std::vector<uint8_t> blob;
            if (kind == 0 || lastBlob.empty()) {
                baseId = (int)r.below(256);
                FuzzJpeg j = randomPicture(r, 320, baseId, MAX_IMAGE_SIZE);
                segs = (unsigned)j.segs.size();
                blob = fuzzAssemble(j);
            } else if (kind == 1) {
                blob = fuzzMakeDelta(r.chance(80) ? baseId : (int)r.below(256), segs,
                                     fuzzDeltaEntries(r, segs, 20, 50));
            } else if (kind == 2) {
                blob = fuzzAssemble(randomPicture(r, r.chance(50) ? 640 : 320, r.chance(50) ? -1 : (int)r.below(256),
                                                  MAX_IMAGE_SIZE));
            } else {
                blob = lastBlob;
                unsigned m = r.range(1, 6);
                for (unsigned k = 0; k < m; k++) fuzzMutate(r, blob);
            }
            if (blob.size() > MAX_IMAGE_SIZE) blob.resize(MAX_IMAGE_SIZE);
            lastBlob = blob;
            unsigned nc = chunksOf(blob.size());
            const char* bdev = r.chance(80) ? CAM : dev;
            for (unsigned c = 0; c < nc; c++) {
                unsigned cc = r.chance(5) ? r.below(nc) : c;            // out of order
                if (r.chance(5)) continue;                              // lost
                chunk(bdev, (uint16_t)cc, chunkLen(blob, cc), cc == 0, cc + 1 == nc, &blob[cc * CHUNK]);
                if (r.chance(3)) chunk(bdev, (uint16_t)cc, chunkLen(blob, cc), cc == 0, cc + 1 == nc, &blob[cc * CHUNK]);
                if (r.chance(1)) chunk(devs[r.below(NDEV)], (uint16_t)c, len, false, false, data);   // interleaved
            }
        } else if (op < 87) {
            imageReceiverSetDrop(r.chance(50) ? 0 : r.below(5));
        } else if (op < 90) {                               // null arguments
            imageReceiverProcessChunk(r.chance(50) ? nullptr : dev, idx, len, true, true, r.chance(50) ? nullptr : data);
            imageReceiverProcessResend(r.chance(50) ? nullptr : dev, idx, len, true, r.chance(50) ? nullptr : data);
            imageReceiverPollSeen(nullptr);
            imageReceiverAfterPollAnswer(nullptr);
            if (r.chance(10)) imageReceiverPublishPending();
        } else if (op < 92) {
            imageReceiverInit();
        } else {                                            // a chunk next to the last valid blob
            if (!lastBlob.empty()) {
                unsigned c = r.below(chunksOf(lastBlob.size()));
                chunk(CAM, (uint16_t)c, chunkLen(lastBlob, c), r.chance(30), r.chance(30), &lastBlob[c * CHUNK]);
            }
        }
        checkOutputs();
        if (fuzzStop("fuzz_image_random_operations", it)) return;
    }
}

/* ---- Deferred publish: finishing a picture stores it, the poll hook or the timer publishes it -- */
TEST(fuzz_image_publish_after_poll_answer) {
    Rng r(0x1A6E0004);
    resetAll();
    hostAdvanceMillis(1000);
    // a colour base, a delta spliced into it, a grayscale flash picture, a large one
    FuzzJpeg base = randomPicture(r, 320, 7, MAX_IMAGE_SIZE);
    std::vector<FuzzDeltaEntry> e = fuzzDeltaEntries(r, (unsigned)base.segs.size(), 30, 60);
    struct Step { std::vector<uint8_t> blob, want; };
    std::vector<Step> steps;
    steps.push_back({fuzzAssemble(base), fuzzAssemble(base)});
    if (!e.empty())                                         // (an empty delta publishes nothing)
        steps.push_back({fuzzMakeDelta(7, (unsigned)base.segs.size(), e), fuzzAssemble(fuzzSplice(base, e))});
    std::vector<uint8_t> gray = fuzzAssemble(randomPicture(r, 320, -1, GRAY_MAX));
    steps.push_back({gray, gray});
    std::vector<uint8_t> large = fuzzAssemble(randomPicture(r, 640, 9, MAX_IMAGE_SIZE));
    steps.push_back({large, large});
    for (const Step& s : steps) {
        const std::vector<uint8_t>& blob = s.blob;
        const std::vector<uint8_t>& want = s.want;
        int before = hostMqtt.images;
        sendBlob(CAM, blob);
        CHECK_EQ(hostMqtt.images, before);                  // (1) finishing does not publish
        imageReceiverAfterPollAnswer("OTHER001");           // another camera's poll: not its picture
        imageReceiverAfterPollAnswer(nullptr);
        CHECK_EQ(hostMqtt.images, before);
        imageReceiverAfterPollAnswer(CAM);                  // (2) the poll answer went out
        CHECK_EQ(hostMqtt.images, before + 1);
        CHECK(hostMqtt.last == want);
        CHECK(hostMqtt.lastDevice == CAM);
        imageReceiverAfterPollAnswer(CAM);                  // published once only
        imageReceiverPublishPending();
        CHECK_EQ(hostMqtt.images, before + 1);
        checkOutputs();
    }
}

TEST(fuzz_image_publish_after_defer_time) {
    Rng r(0x1A6E0005);
    resetAll();
    hostAdvanceMillis(1000);
    std::vector<uint8_t> pic = fuzzAssemble(randomPicture(r, 320, 3, MAX_IMAGE_SIZE));
    sendBlob(CAM, pic);
    CHECK_EQ(hostMqtt.images, 0);
    imageReceiverLoop();
    CHECK_EQ(hostMqtt.images, 0);
    hostAdvanceMillis(IMG_PUBLISH_DEFER_MS - 1);
    imageReceiverLoop();
    CHECK_EQ(hostMqtt.images, 0);                           // (3) not yet
    hostAdvanceMillis(1);
    imageReceiverLoop();
    CHECK_EQ(hostMqtt.images, 1);                           // after the defer time, no poll needed
    CHECK(hostMqtt.last == pic);
    hostAdvanceMillis(5000);
    imageReceiverLoop();
    CHECK_EQ(hostMqtt.images, 1);
}

TEST(fuzz_image_newer_picture_flushes_pending) {
    Rng r(0x1A6E0006);
    resetAll();
    hostAdvanceMillis(1000);
    std::vector<uint8_t> a = fuzzAssemble(randomPicture(r, 320, 1, MAX_IMAGE_SIZE));
    std::vector<uint8_t> b = fuzzAssemble(randomPicture(r, 320, 2, MAX_IMAGE_SIZE));
    sendBlob(CAM, a);
    CHECK_EQ(hostMqtt.images, 0);
    sendBlob(CAM, b);                                       // (4) the pending one goes out first, intact
    CHECK_EQ(hostMqtt.images, 1);
    CHECK(hostMqtt.last == a);
    CHECK(current() == b);
    imageReceiverAfterPollAnswer(CAM);
    CHECK_EQ(hostMqtt.images, 2);
    CHECK(hostMqtt.last == b);
}

/** A picture of 4+ chunks with chunk `lost` (not the first, not the last) missing, sent as the camera
 *  does; then the poll that follows it, answered with the resend request (the order of main.cpp:
 *  imageReceiverPollSeen, answer, imageReceiverAfterPollAnswer). */
static void sendHoleyAndAnswerPoll(Rng& r, std::vector<uint8_t>& pic, unsigned* lost) {
    FuzzJpeg j = fuzzMakeJpeg(r, 320, (int)r.below(256), r.range(20, 200), r.range(5, 45));
    pic = fuzzAssemble(j);
    unsigned nc = chunksOf(pic.size());
    CHECK(nc > 3);
    *lost = nc > 3 ? r.range(1, nc - 2) : 1;
    for (unsigned c = 0; c < nc; c++)
        if (c != *lost) chunk(CAM, (uint16_t)c, chunkLen(pic, c), c == 0, c + 1 == nc, &pic[c * CHUNK]);
    CHECK_EQ(hostMqtt.images, 0);
    hostAdvanceMillis(30);
    imageReceiverPollSeen(CAM);
    uint16_t first;
    uint32_t map;
    CHECK(takeResendRequest(CAM, &first, &map));
    CHECK_EQ((unsigned)first, *lost);
    imageReceiverAfterPollAnswer(CAM);
}

// A picture with a hole is held while its resend runs (a blocking publish would make the camera's
// next poll, ~70 ms after the 0xF0 answer, miss its deadline); the complete picture replaces it and
// is published after the ACK poll answer: one publish, the complete picture.
TEST(fuzz_image_resend_replaces_pending_first_pass) {
    Rng r(0x1A6E0007);
    for (int it = 0; it < fuzzIters(50); it++) {
        resetAll();
        hostAdvanceMillis(1000);
        std::vector<uint8_t> pic;
        unsigned lost;
        sendHoleyAndAnswerPoll(r, pic, &lost);
        CHECK_EQ(hostMqtt.images, 0);                       // held: not published with the 0xF0 answer
        hostAdvanceMillis(IMG_PUBLISH_DEFER_MS + 50);
        imageReceiverLoop();
        CHECK_EQ(hostMqtt.images, 0);                       // nor by the timer while the resend runs
        resend(CAM, (uint16_t)lost, chunkLen(pic, lost), false, &pic[lost * CHUNK]);
        CHECK_EQ(hostMqtt.images, 0);
        hostAdvanceMillis(70);                              // the ACK poll
        imageReceiverPollSeen(CAM);
        uint16_t f;
        uint32_t m;
        CHECK(!takeResendRequest(CAM, &f, &m));
        imageReceiverAfterPollAnswer(CAM);
        CHECK_EQ(hostMqtt.images, 1);
        CHECK(hostMqtt.last == pic);
        CHECK(current() == pic);
        checkOutputs();
    }
}

// A resend that ends without completing: the first pass is then published (by the next loop() or poll hook).
TEST(fuzz_image_resend_given_up_publishes_first_pass) {
    Rng r(0x1A6E0008);
    for (int it = 0; it < fuzzIters(30); it++) {
        for (int how = 0; how < 2; how++) {
            resetAll();
            hostAdvanceMillis(1000);
            std::vector<uint8_t> pic;
            unsigned lost;
            sendHoleyAndAnswerPoll(r, pic, &lost);
            CHECK_EQ(hostMqtt.images, 0);
            size_t hole0 = (size_t)lost * CHUNK, hole1 = hole0 + chunkLen(pic, lost);
            if (how == 0) {                                 // no answer to the request: given up
                hostAdvanceMillis(20001);
                imageReceiverLoop();
            } else {                                        // the camera sends a new picture instead
                std::vector<uint8_t> next = fuzzAssemble(randomPicture(r, 320, 77, MAX_IMAGE_SIZE));
                sendBlob(CAM, next);
                CHECK_EQ(hostMqtt.images, 1);               // finishing it flushed the first pass
            }
            CHECK_EQ(hostMqtt.images, 1);
            CHECK_EQ(hostMqtt.last.size(), pic.size());
            for (size_t i = 0; i < pic.size(); i++)
                if (i < hole0 || i >= hole1) CHECK_EQ(hostMqtt.last[i], pic[i]);
            checkOutputs();
        }
    }
}

// Several pictures finished without a publish in between: the flushes jpegFinish() does (before a
// delta splices into s_jpg, before s_jpg is overwritten, ...) publish every one intact, in order.
TEST(fuzz_image_back_to_back_pictures_publish_in_order) {
    Rng r(0x1A6E0009);
    for (int it = 0; it < fuzzIters(20); it++) {
        resetAll();
        hostMqtt.keepAll = true;
        hostAdvanceMillis(1000);
        std::vector<std::vector<uint8_t>> want;
        FuzzJpeg base = randomPicture(r, 320, 5, MAX_IMAGE_SIZE);
        want.push_back(fuzzAssemble(base));
        sendBlob(CAM, want.back());
        FuzzJpeg cur = base;
        for (int d = 0; d < 2; d++) {                       // two deltas, each into the one before
            std::vector<FuzzDeltaEntry> e = fuzzDeltaEntries(r, (unsigned)cur.segs.size(), 30, 60);
            if (e.empty()) continue;
            std::vector<uint8_t> blob = fuzzMakeDelta(5, (unsigned)cur.segs.size(), e);
            FuzzJpeg next = fuzzSplice(cur, e);
            std::vector<uint8_t> nb = fuzzAssemble(next);
            if (blob.size() > MAX_IMAGE_SIZE || nb.size() > MAX_IMAGE_SIZE) continue;
            sendBlob(CAM, blob);
            cur = next;
            want.push_back(nb);
        }
        want.push_back(fuzzAssemble(randomPicture(r, 320, -1, GRAY_MAX)));     // gray, kept in s_jpgGray
        sendBlob(CAM, want.back());
        for (int k = 0; k < 200; k++) {                                         // gray, too big for it
            std::vector<uint8_t> g = fuzzAssemble(randomPicture(r, 320, -1, MAX_IMAGE_SIZE));
            if (g.size() > GRAY_MAX) {
                want.push_back(g);
                sendBlob(CAM, g);
                break;
            }
        }
        std::vector<uint8_t> hl = fuzzAssemble(randomPicture(r, 320, 9, MAX_IMAGE_SIZE));   // no frame header
        for (size_t i = 2; i + 9 < hl.size() && i < 1024; i++)
            if (hl[i] == 0xFF && hl[i + 1] == 0xC0) hl[i + 1] = 0xC1;
        want.push_back(hl);
        sendBlob(CAM, hl);
        imageReceiverPublishPending();
        CHECK_EQ(hostMqtt.all.size(), want.size());
        for (size_t k = 0; k < want.size() && k < hostMqtt.all.size(); k++) CHECK(hostMqtt.all[k] == want[k]);
        hostMqtt.keepAll = false;
        checkOutputs();
    }
}
