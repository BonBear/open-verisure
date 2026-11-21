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
#include "picture_token.h"
#include <Arduino.h>

void imageReceiverHostTestRestart();    // image_receiver.cpp, HOST_TEST only
void imageReceiverHostTestFreeBuffers();
void imageReceiverHostTestFailAlloc(int n);
uint32_t imageReceiverHostTestNoSlotDrops();

namespace {

const unsigned CHUNK = 40;
const unsigned MAX_CHUNKS = (MAX_IMAGE_SIZE + CHUNK - 1) / CHUNK;
const size_t GRAY_MAX = 16384;          // image_receiver.cpp
const char* CAM = "CAMA0001";
const char* CAM_B = "CAMB0002";

void resetAll() {
    imageReceiverHostTestRestart();
    downlinkInit();
    pictureTokenInit();
    hostMqttReset();
}

/** imageReceiverProcessChunk() with the data in an exactly sized heap block. */
void chunk(const char* dev, uint16_t idx, uint8_t len, bool first, bool last, const uint8_t* src, uint8_t ch = 0) {
    ExactBuf b(len);
    if (len) memcpy(b.p, src, len);
    imageReceiverProcessChunk(dev, idx, len, first, last, b.p, ch);
}

void resend(const char* dev, uint16_t idx, uint8_t len, bool last, const uint8_t* src, uint8_t ch = 0) {
    ExactBuf b(len);
    if (len) memcpy(b.p, src, len);
    imageReceiverProcessResend(dev, idx, len, last, b.p, ch);
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

/** The 320 picture / the large picture of one camera (empty if it has none). */
std::vector<uint8_t> currentOf(const char* dev) {
    size_t len = 0;
    const uint8_t* p = imageReceiverJpeg(&len, nullptr, dev);
    return p ? std::vector<uint8_t>(p, p + len) : std::vector<uint8_t>();
}

/** The pictures one camera returns stay within their buffers. */
void checkCamera(const char* dev) {
    size_t len = 0;
    uint16_t missing = 0;
    const uint8_t* p = imageReceiverJpeg(&len, &missing, dev);
    checkPicture(p, len, missing, MAX_IMAGE_SIZE, true);
    if (!p) CHECK_EQ(len, 0);
    p = imageReceiverJpegLarge(&len, &missing, dev);
    checkPicture(p, len, missing, MAX_IMAGE_SIZE, false);
    if (!p) CHECK_EQ(len, 0);
}

/** Two cameras sending at the same time: their chunks arrive interleaved (each camera in order). */
void sendInterleaved(Rng& r, const char* d0, const std::vector<uint8_t>& b0, const char* d1,
                     const std::vector<uint8_t>& b1) {
    unsigned n0 = chunksOf(b0.size()), n1 = chunksOf(b1.size()), i0 = 0, i1 = 0;
    while (i0 < n0 || i1 < n1) {
        if (i1 >= n1 || (i0 < n0 && r.chance(50))) {
            chunk(d0, (uint16_t)i0, chunkLen(b0, i0), i0 == 0, i0 + 1 == n0, &b0[i0 * CHUNK]);
            i0++;
        } else {
            chunk(d1, (uint16_t)i1, chunkLen(b1, i1), i1 == 0, i1 + 1 == n1, &b1[i1 * CHUNK]);
            i1++;
        }
    }
}

/** Every picture published since the last hostMqttReset() went to its own camera's topic: the
 *  last one of each device equals what that device is expected to have published. */
void checkPublishedOnce(const char* d0, const std::vector<uint8_t>& w0, const char* d1, const std::vector<uint8_t>& w1) {
    CHECK_EQ(hostMqtt.all.size(), 2);
    CHECK_EQ(hostMqtt.allDevice.size(), hostMqtt.all.size());
    int seen0 = 0, seen1 = 0;
    for (size_t k = 0; k < hostMqtt.all.size() && k < hostMqtt.allDevice.size(); k++) {
        if (hostMqtt.allDevice[k] == d0) { seen0++; CHECK(hostMqtt.all[k] == w0); }
        if (hostMqtt.allDevice[k] == d1) { seen1++; CHECK(hostMqtt.all[k] == w1); }
    }
    CHECK_EQ(seen0, 1);
    CHECK_EQ(seen1, 1);
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
    // more valid cameras than slots (IMG_MAX_CAMERAS), so slots are evicted and chunks dropped too
    static const char* devs[] = {"CAMA0001", "CAMB0002", "CAMA0001", "X", "", "CAMC0003LONGERID",
                                 "CAMD0004", "CAME0005", "CAMF0006"};
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
            const char* bdev = r.chance(60) ? CAM : dev;
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
        checkCamera(devs[r.below(NDEV)]);
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

/* ---- Several cameras: every picture state is per camera --------------------------------------- */

// Two cameras send full pictures and deltas at the same time (chunks interleaved): each picture is
// published intact on its own camera's topic, and a delta is spliced into its own camera's base (both
// cameras use the same COM id on purpose, so a mix-up would show).
TEST(fuzz_image_two_cameras_interleaved) {
    Rng r(0x1A6E000A);
    for (int it = 0; it < fuzzIters(40); it++) {
        resetAll();
        hostAdvanceMillis(1000);
        int id = (int)r.below(256);
        FuzzJpeg baseA = randomPicture(r, 320, id, MAX_IMAGE_SIZE);
        FuzzJpeg baseB = randomPicture(r, 320, id, MAX_IMAGE_SIZE);
        std::vector<uint8_t> a = fuzzAssemble(baseA), b = fuzzAssemble(baseB);
        hostMqtt.keepAll = true;
        sendInterleaved(r, CAM, a, CAM_B, b);
        CHECK_EQ(hostMqtt.images, 0);                       // finishing does not publish
        imageReceiverAfterPollAnswer(CAM);
        imageReceiverAfterPollAnswer(CAM_B);
        checkPublishedOnce(CAM, a, CAM_B, b);
        CHECK(currentOf(CAM) == a);
        CHECK(currentOf(CAM_B) == b);
        for (int round = 0; round < 2; round++) {           // deltas, each into its own base
            std::vector<FuzzDeltaEntry> ea, eb;
            for (int k = 0; k < 20 && ea.empty(); k++) ea = fuzzDeltaEntries(r, (unsigned)baseA.segs.size(), 30, 60);
            for (int k = 0; k < 20 && eb.empty(); k++) eb = fuzzDeltaEntries(r, (unsigned)baseB.segs.size(), 30, 60);
            if (ea.empty() || eb.empty()) break;
            std::vector<uint8_t> da = fuzzMakeDelta(id, (unsigned)baseA.segs.size(), ea);
            std::vector<uint8_t> db = fuzzMakeDelta(id, (unsigned)baseB.segs.size(), eb);
            FuzzJpeg nextA = fuzzSplice(baseA, ea), nextB = fuzzSplice(baseB, eb);
            std::vector<uint8_t> na = fuzzAssemble(nextA), nb = fuzzAssemble(nextB);
            if (da.size() > MAX_IMAGE_SIZE || db.size() > MAX_IMAGE_SIZE || na.size() > MAX_IMAGE_SIZE ||
                nb.size() > MAX_IMAGE_SIZE) break;
            hostMqttReset();
            hostMqtt.keepAll = true;
            hostAdvanceMillis(r.range(100, 3000));
            sendInterleaved(r, CAM, da, CAM_B, db);
            CHECK_EQ(hostMqtt.images, 0);
            imageReceiverAfterPollAnswer(CAM_B);
            imageReceiverAfterPollAnswer(CAM);
            checkPublishedOnce(CAM, na, CAM_B, nb);
            CHECK(currentOf(CAM) == na);
            CHECK(currentOf(CAM_B) == nb);
            baseA = nextA;
            baseB = nextB;
            checkOutputs();
        }
        // "newest" follows the camera whose picture changed last; a large picture belongs to its camera only
        std::vector<uint8_t> a2 = fuzzAssemble(randomPicture(r, 320, 3, MAX_IMAGE_SIZE));
        sendBlobPublished(CAM, a2);
        CHECK(current() == a2);
        std::vector<uint8_t> b2 = fuzzAssemble(randomPicture(r, 320, 4, MAX_IMAGE_SIZE));
        sendBlobPublished(CAM_B, b2);
        CHECK(current() == b2);
        CHECK(currentOf(CAM) == a2);
        std::vector<uint8_t> big = fuzzAssemble(randomPicture(r, 640, 5, MAX_IMAGE_SIZE));
        sendBlobPublished(CAM, big);
        CHECK(currentLarge() == big);
        CHECK(imageReceiverJpegLarge(nullptr, nullptr, CAM_B) == nullptr);
        CHECK(imageReceiverJpegLarge(nullptr, nullptr, CAM) != nullptr);
        CHECK(currentOf(CAM_B) == b2);                      // the large picture did not touch it
        CHECK(imageReceiverJpeg(nullptr, nullptr, "CAMZ0009") == nullptr);   // unknown camera
        checkCamera(CAM);
        checkCamera(CAM_B);
        if (fuzzStop("fuzz_image_two_cameras_interleaved", it)) return;
    }
}

// Camera A has a picture with two holes and its resend running (the first pass is held). Camera B
// sends a whole picture meanwhile, A's resent chunks arriving in between: A's resend goes on and
// completes, nothing of A is reset or cancelled, and each picture is published once, intact, for its camera.
TEST(fuzz_image_resend_continues_while_other_camera_sends) {
    Rng r(0x1A6E000B);
    for (int it = 0; it < fuzzIters(40); it++) {
        resetAll();
        hostAdvanceMillis(1000);
        FuzzJpeg ja = fuzzMakeJpeg(r, 320, (int)r.below(256), r.range(60, 200), r.range(5, 45));   // 6+ chunks
        std::vector<uint8_t> a = fuzzAssemble(ja);
        unsigned nc = chunksOf(a.size());
        REQUIRE(nc > 5);
        unsigned l1 = r.range(1, nc - 3), l2 = l1 + 1;
        for (unsigned c = 0; c < nc; c++)
            if (c != l1 && c != l2) chunk(CAM, (uint16_t)c, chunkLen(a, c), c == 0, c + 1 == nc, &a[c * CHUNK]);
        hostAdvanceMillis(30);
        imageReceiverPollSeen(CAM);
        uint16_t first;
        uint32_t map;
        CHECK(takeResendRequest(CAM, &first, &map));
        CHECK_EQ((unsigned)first, l1);
        CHECK_EQ(map, 3);
        imageReceiverAfterPollAnswer(CAM);
        CHECK_EQ(hostMqtt.images, 0);                       // held while the resend runs

        std::vector<uint8_t> b = fuzzAssemble(randomPicture(r, 320, (int)r.below(256), MAX_IMAGE_SIZE));
        unsigned nb = chunksOf(b.size());
        bool d1 = false, d2 = false;
        for (unsigned c = 0; c < nb; c++) {
            chunk(CAM_B, (uint16_t)c, chunkLen(b, c), c == 0, c + 1 == nb, &b[c * CHUNK]);
            if (!d1 && c >= nb / 3) {
                resend(CAM, (uint16_t)l1, chunkLen(a, l1), false, &a[l1 * CHUNK]);
                d1 = true;
            } else if (d1 && !d2 && c >= 2 * nb / 3) {
                resend(CAM, (uint16_t)l2, chunkLen(a, l2), false, &a[l2 * CHUNK]);
                d2 = true;
            }
        }
        if (!d1) resend(CAM, (uint16_t)l1, chunkLen(a, l1), false, &a[l1 * CHUNK]);
        if (!d2) resend(CAM, (uint16_t)l2, chunkLen(a, l2), false, &a[l2 * CHUNK]);
        CHECK_EQ(hostMqtt.images, 0);                       // neither finishing nor completing publishes
        CHECK(!takeResendRequest(CAM, &first, &map));       // A's resend is complete
        CHECK(currentOf(CAM) == a);
        CHECK(currentOf(CAM_B) == b);
        hostAdvanceMillis(70);                              // A's ACK poll, B's poll
        imageReceiverPollSeen(CAM);
        CHECK(!takeResendRequest(CAM, &first, &map));
        imageReceiverPollSeen(CAM_B);
        imageReceiverAfterPollAnswer(CAM_B);
        CHECK_EQ(hostMqtt.images, 1);
        CHECK(hostMqtt.lastDevice == CAM_B);
        CHECK(hostMqtt.last == b);
        imageReceiverAfterPollAnswer(CAM);
        CHECK_EQ(hostMqtt.images, 2);                       // the complete picture only, not the first pass too
        CHECK(hostMqtt.lastDevice == CAM);
        CHECK(hostMqtt.last == a);
        checkOutputs();
        if (fuzzStop("fuzz_image_resend_continues_while_other_camera_sends", it)) return;
    }
}

// More cameras than slots: a new camera takes a free slot, else the idle slot (no open picture, no
// resend, no pending publish) that has been quiet the longest; with no idle slot its chunks are dropped.
TEST(fuzz_image_slot_eviction) {
    static_assert(IMG_MAX_CAMERAS == 4, "the test uses 4 slots and 6 cameras");
    Rng r(0x1A6E000C);
    static const char* cams[] = {"CAMC0000", "CAMC0001", "CAMC0002", "CAMC0003", "CAMC0004", "CAMC0005"};
    for (int it = 0; it < fuzzIters(20); it++) {
        std::vector<uint8_t> pic[6], pic2[6];
        for (int k = 0; k < 6; k++) {
            pic[k] = fuzzAssemble(randomPicture(r, 320, 10 + k, MAX_IMAGE_SIZE));
            pic2[k] = fuzzAssemble(randomPicture(r, 320, 20 + k, MAX_IMAGE_SIZE));
        }
        // 1. the fifth camera evicts the camera that has been quiet the longest
        resetAll();
        hostAdvanceMillis(1000);
        for (int k = 0; k < 4; k++) {
            sendBlobPublished(cams[k], pic[k]);
            hostAdvanceMillis(1000);
        }
        for (int k = 0; k < 4; k++) CHECK(currentOf(cams[k]) == pic[k]);
        sendBlobPublished(cams[4], pic[4]);
        hostAdvanceMillis(1000);
        CHECK(currentOf(cams[0]).empty());
        CHECK(imageReceiverJpeg(nullptr, nullptr, cams[0]) == nullptr);
        for (int k = 1; k <= 4; k++) CHECK(currentOf(cams[k]) == pic[k]);
        CHECK_EQ(imageReceiverHostTestNoSlotDrops(), 0);

        // 2. a camera with an open picture is not evicted, even when it is the oldest
        std::vector<uint8_t>& p1 = pic2[1];
        unsigned n1 = chunksOf(p1.size());
        REQUIRE(n1 >= 3);
        for (unsigned c = 0; c < 2; c++) chunk(cams[1], (uint16_t)c, chunkLen(p1, c), c == 0, false, &p1[c * CHUNK]);
        hostAdvanceMillis(3000);
        for (int k = 2; k <= 4; k++) {
            sendBlobPublished(cams[k], pic2[k]);
            hostAdvanceMillis(10);
        }
        sendBlobPublished(cams[5], pic[5]);                 // the oldest idle slot is camera 2's
        CHECK(currentOf(cams[2]).empty());
        CHECK(currentOf(cams[5]) == pic[5]);
        CHECK(currentOf(cams[3]) == pic2[3]);
        CHECK(currentOf(cams[4]) == pic2[4]);
        for (unsigned c = 2; c < n1; c++) chunk(cams[1], (uint16_t)c, chunkLen(p1, c), false, c + 1 == n1, &p1[c * CHUNK]);
        imageReceiverAfterPollAnswer(cams[1]);
        CHECK(hostMqtt.lastDevice == cams[1]);
        CHECK(hostMqtt.last == p1);                         // finished intact
        CHECK(currentOf(cams[1]) == p1);
        CHECK_EQ(imageReceiverHostTestNoSlotDrops(), 0);

        // 3. every slot busy (a picture waits for its publish): the chunks are dropped and counted
        resetAll();
        hostAdvanceMillis(1000);
        hostMqtt.keepAll = true;
        for (int k = 0; k < 4; k++) {
            sendBlob(cams[k], pic[k]);
            hostAdvanceMillis(10);
        }
        CHECK_EQ(hostMqtt.images, 0);
        sendBlob(cams[4], pic[4]);
        CHECK_EQ(imageReceiverHostTestNoSlotDrops(), chunksOf(pic[4].size()));
        CHECK_EQ(hostMqtt.images, 0);
        CHECK(currentOf(cams[4]).empty());
        imageReceiverPublishPending();                      // the oldest first, each to its own camera
        REQUIRE(hostMqtt.all.size() == 4);
        for (size_t k = 0; k < 4; k++) {
            CHECK(hostMqtt.allDevice[k] == cams[k]);
            CHECK(hostMqtt.all[k] == pic[k]);
        }
        sendBlobPublished(cams[4], pic[4]);                 // idle now: takes the slot of the oldest
        CHECK(currentOf(cams[4]) == pic[4]);
        CHECK(currentOf(cams[0]).empty());
        checkOutputs();
        if (fuzzStop("fuzz_image_slot_eviction", it)) return;
    }
}

// A camera whose buffers cannot be allocated loses its pictures (logged, retried after a minute); the
// other cameras and the next ones still work. n: the failing allocation is the splice buffer, imgBuf or jpg of the first camera.
TEST(fuzz_image_allocation_failure_of_one_slot) {
    Rng r(0x1A6E000D);
    for (int n = 0; n < 3; n++) {
        imageReceiverHostTestFreeBuffers();
        resetAll();
        hostAdvanceMillis(1000);
        std::vector<uint8_t> a = fuzzAssemble(randomPicture(r, 320, 1, MAX_IMAGE_SIZE));
        std::vector<uint8_t> b = fuzzAssemble(randomPicture(r, 320, 2, MAX_IMAGE_SIZE));
        imageReceiverHostTestFailAlloc(n);
        sendBlob(CAM, a);
        imageReceiverAfterPollAnswer(CAM);
        imageReceiverPublishPending();
        CHECK_EQ(hostMqtt.images, 0);
        CHECK(currentOf(CAM).empty());
        checkOutputs();
        sendBlobPublished(CAM_B, b);                        // another camera is not affected
        CHECK(hostMqtt.lastDevice == CAM_B);
        CHECK(hostMqtt.last == b);
        CHECK(currentOf(CAM_B) == b);
        sendBlob(CAM, a);                                   // still dropped, and B keeps its picture
        imageReceiverPublishPending();
        CHECK_EQ(hostMqtt.images, 1);
        CHECK(currentOf(CAM).empty());
        CHECK(currentOf(CAM_B) == b);
        hostAdvanceMillis(59000);                           // not yet: retried once per minute only
        sendBlob(CAM, a);
        imageReceiverPublishPending();
        CHECK_EQ(hostMqtt.images, 1);
        CHECK(currentOf(CAM).empty());
        hostAdvanceMillis(1500);                            // the next picture of the camera allocates again
        sendBlobPublished(CAM, a);
        CHECK_EQ(hostMqtt.images, 2);
        CHECK(hostMqtt.lastDevice == CAM);
        CHECK(hostMqtt.last == a);
        CHECK(currentOf(CAM) == a);
        CHECK(currentOf(CAM_B) == b);
        checkOutputs();
        if (fuzzStop("fuzz_image_allocation_failure_of_one_slot", n)) return;
    }
    imageReceiverHostTestFreeBuffers();                     // the next tests allocate again
    resetAll();
}

// The reassembly buffer is the resend's buffer too: after a resend was cancelled by the camera's next
// picture, a chunk of that picture that never arrives must be zero (a hole), not the old picture's
// bytes (the wipe in imageReceiverProcessChunk, imgDirty). The held first pass goes out intact.
TEST(fuzz_image_new_picture_after_cancelled_resend_has_clean_holes) {
    Rng r(0x1A6E000E);
    int tested = 0;
    for (int it = 0; it < fuzzIters(60); it++) {
        resetAll();
        hostAdvanceMillis(1000);
        std::vector<uint8_t> old;
        unsigned lost;
        sendHoleyAndAnswerPoll(r, old, &lost);              // the resend runs, the first pass is held
        CHECK_EQ(hostMqtt.images, 0);
        int id = (int)r.below(256);
        std::vector<uint8_t> np = fuzzAssemble(fuzzMakeJpeg(r, 320, id, r.range(30, 100), r.range(5, 45)));
        unsigned nOld = chunksOf(old.size()), nNew = chunksOf(np.size());
        unsigned m = nOld < nNew ? nOld : nNew;
        if (m < 5) continue;                                // (hole after the header chunks, inside both pictures)
        unsigned h = r.range(3, m - 2);
        if (h == lost) continue;
        hostAdvanceMillis(100);
        for (unsigned c = 0; c < nNew; c++)
            if (c != h) chunk(CAM, (uint16_t)c, chunkLen(np, c), c == 0, c + 1 == nNew, &np[c * CHUNK]);
        CHECK_EQ(hostMqtt.images, 1);                       // finishing flushed the first pass, intact
        CHECK_EQ(hostMqtt.last.size(), old.size());
        size_t hole0 = (size_t)lost * CHUNK, hole1 = hole0 + chunkLen(old, lost);
        for (size_t i = 0; i < old.size(); i++)
            if (i < hole0 || i >= hole1) CHECK_EQ(hostMqtt.last[i], old[i]);
        std::vector<uint8_t> got = currentOf(CAM);          // the new picture with a zero hole in chunk h
        CHECK_EQ(got.size(), np.size());
        size_t n0 = (size_t)h * CHUNK, n1 = n0 + chunkLen(np, h);
        for (size_t i = 0; i < got.size() && i < np.size(); i++) CHECK_EQ(got[i], (i >= n0 && i < n1) ? 0 : np[i]);
        tested++;
        checkOutputs();
        if (fuzzStop("fuzz_image_new_picture_after_cancelled_resend_has_clean_holes", it)) return;
    }
    CHECK(tested > 0);
}

/** Pictures whose frame header (SOF0) is not recognised: stored nowhere, published from a copy. */
static std::vector<uint8_t> headerless(const std::vector<uint8_t>& pic) {
    std::vector<uint8_t> v = pic;
    for (size_t i = 2; i + 9 < v.size() && i < 1024; i++)
        if (v[i] == 0xFF && v[i + 1] == 0xC0) v[i + 1] = 0xC1;
    return v;
}

// Pictures kept in a copy buffer (no frame header) are pending per camera: one camera finishing a
// picture never publishes another camera's pending or held picture (that blocking publish would run
// before the other camera's poll answer and ignore its hold); each goes out after its own poll answer.
TEST(fuzz_image_headerless_pictures_of_two_cameras_publish_separately) {
    Rng r(0x1A6E000F);
    for (int it = 0; it < fuzzIters(30); it++) {
        // interleaved, both complete
        resetAll();
        hostAdvanceMillis(1000);
        hostMqtt.keepAll = true;
        std::vector<uint8_t> a = headerless(fuzzAssemble(randomPicture(r, 320, 1, MAX_IMAGE_SIZE)));
        std::vector<uint8_t> b = headerless(fuzzAssemble(randomPicture(r, 320, 2, MAX_IMAGE_SIZE)));
        sendInterleaved(r, CAM, a, CAM_B, b);
        CHECK_EQ(hostMqtt.images, 0);                       // nothing published by either last chunk
        imageReceiverAfterPollAnswer(CAM_B);
        CHECK_EQ(hostMqtt.images, 1);
        CHECK(hostMqtt.lastDevice == CAM_B);
        CHECK(hostMqtt.last == b);
        imageReceiverAfterPollAnswer(CAM);
        checkPublishedOnce(CAM, a, CAM_B, b);
        CHECK(currentOf(CAM).empty());                      // not stored without a frame header
        CHECK(currentOf(CAM_B).empty());

        // A's picture has a hole and its resend runs (held); B finishes a picture meanwhile
        resetAll();
        hostAdvanceMillis(1000);
        std::vector<uint8_t> pic =
            headerless(fuzzAssemble(fuzzMakeJpeg(r, 320, 3, r.range(60, 200), r.range(5, 45))));   // 6+ chunks
        unsigned nc = chunksOf(pic.size());
        REQUIRE(nc > 5);
        unsigned lost = r.range(3, nc - 2);
        for (unsigned c = 0; c < nc; c++)
            if (c != lost) chunk(CAM, (uint16_t)c, chunkLen(pic, c), c == 0, c + 1 == nc, &pic[c * CHUNK]);
        hostAdvanceMillis(30);
        imageReceiverPollSeen(CAM);
        uint16_t first;
        uint32_t map;
        CHECK(takeResendRequest(CAM, &first, &map));
        CHECK_EQ((unsigned)first, lost);
        imageReceiverAfterPollAnswer(CAM);
        CHECK_EQ(hostMqtt.images, 0);                       // held
        std::vector<uint8_t> b2 = headerless(fuzzAssemble(randomPicture(r, 320, 4, MAX_IMAGE_SIZE)));
        sendBlob(CAM_B, b2);
        CHECK_EQ(hostMqtt.images, 0);                       // A's held picture was not published by B
        imageReceiverAfterPollAnswer(CAM_B);
        CHECK_EQ(hostMqtt.images, 1);
        CHECK(hostMqtt.lastDevice == CAM_B);
        CHECK(hostMqtt.last == b2);
        hostAdvanceMillis(70);
        resend(CAM, (uint16_t)lost, chunkLen(pic, lost), false, &pic[lost * CHUNK]);
        imageReceiverPollSeen(CAM);
        CHECK(!takeResendRequest(CAM, &first, &map));
        imageReceiverAfterPollAnswer(CAM);
        CHECK_EQ(hostMqtt.images, 2);                       // only the complete picture of A
        CHECK(hostMqtt.lastDevice == CAM);
        CHECK(hostMqtt.last == pic);
        checkOutputs();
        if (fuzzStop("fuzz_image_headerless_pictures_of_two_cameras_publish_separately", it)) return;
    }
}

// A slot whose resend is running is busy even when nothing is pending (here the first pass was
// published by the test API), so it is not evicted although it is the oldest; the resend then completes.
TEST(fuzz_image_slot_with_running_resend_is_not_evicted) {
    static_assert(IMG_MAX_CAMERAS == 4, "the test uses 4 slots and 5 cameras");
    Rng r(0x1A6E0010);
    static const char* others[] = {"CAMC0001", "CAMC0002", "CAMC0003", "CAMC0004"};
    for (int it = 0; it < fuzzIters(30); it++) {
        resetAll();
        hostAdvanceMillis(1000);
        std::vector<uint8_t> pic;
        unsigned lost;
        sendHoleyAndAnswerPoll(r, pic, &lost);              // CAM: resend running, first pass held
        imageReceiverPublishPending();                      // published: pending is gone, the resend goes on
        CHECK_EQ(hostMqtt.images, 1);
        std::vector<uint8_t> p[4];
        for (int k = 0; k < 4; k++) p[k] = fuzzAssemble(randomPicture(r, 320, 30 + k, MAX_IMAGE_SIZE));
        for (int k = 0; k < 3; k++) {                       // the other three slots
            hostAdvanceMillis(1000);
            sendBlobPublished(others[k], p[k]);
        }
        hostAdvanceMillis(1000);
        sendBlobPublished(others[3], p[3]);                 // no free slot: the oldest idle one (not CAM's) goes
        CHECK(currentOf(others[0]).empty());
        for (int k = 1; k < 4; k++) CHECK(currentOf(others[k]) == p[k]);
        CHECK_EQ(imageReceiverHostTestNoSlotDrops(), 0);
        int before = hostMqtt.images;
        resend(CAM, (uint16_t)lost, chunkLen(pic, lost), false, &pic[lost * CHUNK]);   // the resend completes
        imageReceiverAfterPollAnswer(CAM);
        CHECK_EQ(hostMqtt.images, before + 1);
        CHECK(hostMqtt.lastDevice == CAM);
        CHECK(hostMqtt.last == pic);
        CHECK(currentOf(CAM) == pic);
        checkOutputs();
        if (fuzzStop("fuzz_image_slot_with_running_resend_is_not_evicted", it)) return;
    }
}

// Per-camera PCH miss detector: 2 missed pictures -> suppressed 15 min; 2 more after expiry -> 30 min ->
// doubles up to 4 h cap; receiving a chunk on channel 1 resets miss count, backoff and suppression.
TEST(fuzz_image_pch_miss_detector) {
    const unsigned long MIN_MS = 60UL * 1000UL;
    const unsigned long HOUR_MS = 60UL * MIN_MS;
    Rng r(0x1A6E0011);
    std::vector<uint8_t> pic = fuzzAssemble(randomPicture(r, 320, 1, MAX_IMAGE_SIZE));
    unsigned nc = chunksOf(pic.size());
    REQUIRE(nc >= 2);

    auto sendPicOnCh = [&](const char* dev, uint8_t ch) {
        for (unsigned c = 0; c < nc; c++)
            chunk(dev, (uint16_t)c, chunkLen(pic, c), c == 0, c + 1 == nc, &pic[c * CHUNK], ch);
    };

    // 1. Two missed pictures (deaf channel: 0 chunks, finalized on poll) -> suppressed for 15 min
    resetAll();
    hostAdvanceMillis(1000);
    CHECK(!imageReceiverPchSuppressed(CAM));
    imageReceiverNotePicStart(CAM, true);
    imageReceiverPollSeen(CAM);                 // miss 1
    CHECK(!imageReceiverPchSuppressed(CAM));
    imageReceiverNotePicStart(CAM, true);
    imageReceiverPollSeen(CAM);                 // miss 2 -> 15 min suppression
    CHECK(imageReceiverPchSuppressed(CAM));
    CHECK(!imageReceiverPchSuppressed(CAM_B));  // per-camera isolation

    hostAdvanceMillis(15 * MIN_MS - 1);
    CHECK(imageReceiverPchSuppressed(CAM));
    hostAdvanceMillis(1);
    CHECK(!imageReceiverPchSuppressed(CAM));

    // 2. Exponential backoff on repeated misses after expiry: 30m -> 60m -> 120m -> 240m (4h) -> 4h cap
    const unsigned long expectedBackoff[] = {30 * MIN_MS, 60 * MIN_MS, 120 * MIN_MS, 4 * HOUR_MS, 4 * HOUR_MS};
    for (unsigned long waitMs : expectedBackoff) {
        // Test both deaf-channel (NotePicStart + PollSeen) and main-channel fallback (channel 0 chunks)
        imageReceiverNotePicStart(CAM, true);
        sendPicOnCh(CAM, 0);
        CHECK(!imageReceiverPchSuppressed(CAM));
        imageReceiverNotePicStart(CAM, true);
        imageReceiverPollSeen(CAM);
        CHECK(imageReceiverPchSuppressed(CAM));

        hostAdvanceMillis(waitMs - 1);
        CHECK(imageReceiverPchSuppressed(CAM));
        hostAdvanceMillis(1);
        CHECK(!imageReceiverPchSuppressed(CAM));
    }

    // 3. Receiving a chunk on channel 1 resets miss count, backoff and suppression
    // First accumulate 1 miss after an expired 15 min backoff, then succeed on channel 1
    resetAll();
    hostAdvanceMillis(1000);
    for (int i = 0; i < 2; i++) {
        imageReceiverNotePicStart(CAM, true);
        imageReceiverPollSeen(CAM);
    }
    CHECK(imageReceiverPchSuppressed(CAM));
    hostAdvanceMillis(15 * MIN_MS);
    CHECK(!imageReceiverPchSuppressed(CAM));    // backoff is now 15 min (next miss would double to 30 min)

    imageReceiverNotePicStart(CAM, true);
    imageReceiverPollSeen(CAM);                 // 1 miss
    CHECK(!imageReceiverPchSuppressed(CAM));
    imageReceiverNotePollAnswer(CAM, true);
    imageReceiverNotePicStart(CAM);             // 1-arg overload using last poll answer
    sendPicOnCh(CAM, 1);                        // succeeds on PCH -> resets misses & backoff
    imageReceiverPollSeen(CAM);
    CHECK(!imageReceiverPchSuppressed(CAM));

    // Next 2 misses must start at 15 min again (not 30 min)
    imageReceiverNotePicStart(CAM, true);
    imageReceiverNotePicStart(CAM, true);       // finalizes first miss, starts second
    imageReceiverPollSeen(CAM);                 // finalizes second miss
    CHECK(imageReceiverPchSuppressed(CAM));
    hostAdvanceMillis(15 * MIN_MS);
    CHECK(!imageReceiverPchSuppressed(CAM));

    // 4. Stats and manual reset ('pch on')
    PchCamStats stats[IMG_MAX_CAMERAS];
    int nStats = imageReceiverPchGetStats(stats, IMG_MAX_CAMERAS);
    CHECK_EQ(nStats, 1);
    CHECK_STR(stats[0].deviceId, CAM);
    CHECK_EQ(stats[0].pchChunks, nc);
    imageReceiverNotePicStart(CAM, true);
    imageReceiverPollSeen(CAM);
    imageReceiverNotePicStart(CAM, true);
    imageReceiverPollSeen(CAM);
    CHECK(imageReceiverPchSuppressed(CAM));
    imageReceiverPchReset();
    CHECK(!imageReceiverPchSuppressed(CAM));

    imageReceiverPublishPending();
}

// Phase D Picture Token integration with image_receiver: resend renewal, 0xB1 release promoting
// a queued waiter, and legacy camera displacement / immediate release on picture completion.
TEST(fuzz_image_picture_token_integration) {
    Rng r(0x1A6E0012);
    std::vector<uint8_t> picA = fuzzAssemble(fuzzMakeJpeg(r, 320, 1, r.range(60, 140), r.range(5, 40)));
    std::vector<uint8_t> picC = fuzzAssemble(randomPicture(r, 320, 2, MAX_IMAGE_SIZE));
    unsigned nc = chunksOf(picA.size());
    REQUIRE(nc > 4);
    unsigned lost = 2;

    resetAll();
    hostAdvanceMillis(1000);
    PicTokenStatus st;

    // 1. CAM acquires token, sends 0xB2 and picture with a hole -> resend keeps token held
    REQUIRE(pictureTokenRequest(CAM, (uint32_t)millis()));
    pictureTokenOnPicStart(CAM, (uint32_t)millis());
    for (unsigned c = 0; c < nc; c++) {
        hostAdvanceMillis(10);
        if (c != lost) chunk(CAM, (uint16_t)c, chunkLen(picA, c), c == 0, c + 1 == nc, &picA[c * CHUNK], 1);
    }
    hostAdvanceMillis(30);
    imageReceiverPollSeen(CAM);
    uint16_t first = 0;
    uint32_t map = 0;
    REQUIRE(takeResendRequest(CAM, &first, &map));
    CHECK_EQ((unsigned)first, lost);
    pictureTokenOnPollAnswered(CAM, true, (uint32_t)millis());
    imageReceiverAfterPollAnswer(CAM);

    // CAM_B requests token -> held & queued
    CHECK(!pictureTokenRequest(CAM_B, (uint32_t)millis()));
    pictureTokenGetStatus((uint32_t)millis(), &st);
    CHECK_EQ(st.state, PIC_TOK_HELD);
    CHECK_STR(st.holderId, CAM);
    CHECK(!st.holderLegacy);
    CHECK_EQ(st.waitCount, 1);
    CHECK_STR(st.waitIds[0], CAM_B);

    // CAM resends lost chunk -> completes picture, still held until ACK/0xB1
    hostAdvanceMillis(50);
    resend(CAM, (uint16_t)lost, chunkLen(picA, lost), false, &picA[lost * CHUNK], 1);
    imageReceiverPollSeen(CAM);
    CHECK(!takeResendRequest(CAM, &first, &map));
    pictureTokenOnPollAnswered(CAM, false, (uint32_t)millis());
    imageReceiverAfterPollAnswer(CAM);
    CHECK(currentOf(CAM) == picA);

    pictureTokenGetStatus((uint32_t)millis(), &st);
    CHECK_EQ(st.state, PIC_TOK_HELD);
    CHECK_STR(st.holderId, CAM);

    // Explicit 0xB1 release promotes CAM_B to RESERVED
    CHECK(pictureTokenRelease(CAM, (uint32_t)millis()));
    pictureTokenGetStatus((uint32_t)millis(), &st);
    CHECK_EQ(st.state, PIC_TOK_RESERVED);
    CHECK_STR(st.holderId, CAM_B);
    CHECK_EQ(st.waitCount, 0);

    // 2. Legacy camera ("CAMC0003") starts without 0xBB -> displaces CAM_B to front, then
    // releases immediately when jpegFinish completes with rsActive == false.
    const char* LEG = "CAMC0003";
    pictureTokenOnPicStart(LEG, (uint32_t)millis());
    pictureTokenGetStatus((uint32_t)millis(), &st);
    CHECK_EQ(st.state, PIC_TOK_HELD);
    CHECK_STR(st.holderId, LEG);
    CHECK(st.holderLegacy);
    CHECK_EQ(st.waitCount, 1);
    CHECK_STR(st.waitIds[0], CAM_B);

    sendBlob(LEG, picC);
    imageReceiverAfterPollAnswer(LEG);
    CHECK(currentOf(LEG) == picC);

    pictureTokenGetStatus((uint32_t)millis(), &st);
    CHECK_EQ(st.state, PIC_TOK_RESERVED);
    CHECK_STR(st.holderId, CAM_B);
    CHECK_EQ(st.waitCount, 0);
    CHECK(pictureTokenRequest(CAM_B, (uint32_t)millis()));
    pictureTokenRelease(CAM_B, (uint32_t)millis());
    imageReceiverPublishPending();
    checkOutputs();
}
