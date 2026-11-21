/**
 * @file image_receiver.cpp
 * @brief RF Camera Image Chunk Reassembly & MQTT Publishing Implementation
 */

#include "image_receiver.h"
#include "mqtt_manager.h"
#include <string.h>
#include <esp_heap_caps.h>
#include "jpeg_splice.h"
#include "downlink.h"

/* --------------------------- JPEG chunks (type 0x82) ---------------------------
 * Chunk n carries bytes 40n .. 40n+len-1 of the JPEG (40-byte chunks in 56-byte packets), the
 * last one shorter. Chunks are placed by number, so a lost packet only leaves an 8-byte
 * hole (zeros); the sender puts a restart marker after every MCU (or MCU row), so a hole
 * only damages that part. A blob is finished by its last chunk, or by the first chunk of
 * the next blob. Full 320x240 pictures (COM id) become the base that later delta blobs
 * (only the changed MCUs, see jpeg_splice.h) are spliced into; the current picture is
 * published on MQTT and served on /api/camera.jpg. Pictures wider than 320 (640x480) are
 * kept apart for /api/camera_large.jpg.
 *
 * All picture state is per camera (struct CamPic, IMG_MAX_CAMERAS slots): chunks of several
 * cameras may arrive interleaved, and a chunk of one camera never touches the pictures, the delta
 * base or the resend of another. A slot is given to a camera by its first authenticated chunk. */
#define JPEG_CHUNK 40        // data bytes per picture packet (type 0x82/0x83)
#define JPEG_MAX_CHUNKS ((MAX_IMAGE_SIZE + JPEG_CHUNK - 1) / JPEG_CHUNK)
#define HAVE_BYTES ((JPEG_MAX_CHUNKS + 7) / 8)
#define JPEG_QUIET_MS 5000   // the camera sends chunks back to back: a pause this long ends a blob
// grayscale picture without COM id (a flash picture in the dark): kept apart from the colour
// base so the next delta still applies, and served by /api/camera.jpg while it is the newest
#define GRAY_MAX 16384

// Resend of missing chunks (56-byte packets only): a full picture with holes (or a lost end)
// is held for publishing (see publishHeld) and stays in the slot's reassembly buffer imgBuf, which
// then collects the resent chunks (a new chunk of that camera cancels the resend first, and
// imgDirty makes it wipe the buffer); with its next poll answer the gateway asks
// the camera for the missing chunks among the 32 from the first missing one (a bitmap). The
// camera sends them as type 0x83, they are patched into imgBuf, and the next ones are asked for, until the picture is
// complete: then it goes through jpegFinish() again (stored like a picture without holes; it
// replaces the held first pass and is published after the next poll answer). If the resend ends
// without completing, the first pass is published instead.
// Limits: RS_MAX_ROUNDS requests, RS_TIMEOUT_MS without an answer, RS_TOTAL_MS.
#define RS_MAX_ROUNDS 40
#define RS_TIMEOUT_MS 20000
#define RS_TOTAL_MS 60000

// A delta that cannot be applied (the gateway restarted, or missed the full picture it
// belongs to) is useless, and the camera keeps sending deltas against its own reference
// while the Blackfin stays powered. Ask it for a full picture (0xD1) with its next poll,
// at most once per BASE_REQUEST_MS (per camera) so a camera that cannot deliver it is not flooded.
#define BASE_REQUEST_MS 60000UL

// Deferred publish: publishing a picture (up to ~35 KB over MQTT) blocks for 100+ ms, and the
// camera polls right after its last chunk and listens for the answer for ~0.1 s. So jpegFinish()
// only stores the picture and marks it in the camera's slot (pubActive); it is published after that
// camera's poll answer (imageReceiverAfterPollAnswer), or from imageReceiverLoop()
// IMG_PUBLISH_DEFER_MS later if no poll comes. pubBuf points to a buffer that stays unchanged until
// the publish (the slot's jpg, jpgLarge, jpgGray or pubImg, never imgBuf which
// jpegReset() clears): every jpegFinish() of a camera first flushes that camera's pending picture.
#define ALLOC_RETRY_MS 60000UL       // a camera whose buffers could not be allocated is retried this often
static_assert(IMG_MAX_CAMERAS == DOWNLINK_MAX_CAMERAS, "picture slots and downlink slots are per camera: keep equal");

struct CamPic {
    bool used;                       // slot belongs to deviceId
    bool allocFailed;                // its buffers could not be allocated: its pictures are dropped
    unsigned long allocFailTime;     // when, for the retry
    char deviceId[10];
    unsigned long lastUse;           // last chunk, for evicting the oldest idle slot

    // big buffers (PSRAM), allocated on first use and kept for good, also when the slot moves to another camera
    uint8_t* imgBuf;                 // picture being received (and being completed by resend)
    uint8_t* jpg;                    // last finished picture
    uint8_t* jpgLarge;               // last finished picture wider than 320
    uint8_t* jpgGray;                // last grayscale picture
    uint8_t* pubImg;                 // copy for the publish of a picture stored nowhere else (rare)

    // reassembly
    uint8_t have[HAVE_BYTES];        // bit per chunk received
    size_t imgEnd;                   // bytes up to the highest chunk seen
    uint16_t haveCount;
    int32_t lastIdx;
    unsigned long lastChunkTime;
    bool imgDirty;                   // imgBuf holds a picture under resend: wipe it before reuse

    // stored pictures
    size_t jpgLen;
    uint16_t jpgMissing;
    size_t jpgLargeLen;
    uint16_t jpgLargeMissing;
    size_t jpgGrayLen;
    uint16_t jpgGrayMissing;
    bool grayNewest;
    int baseId;                      // COM id of the full picture deltas apply to
    uint16_t deltaCount;             // deltas applied since the last full picture
    uint32_t seq320, seqLarge;       // s_seq when the 320 / large picture last changed
    bool baseRequested;
    unsigned long baseReqTime;

    // resend
    bool rsActive;
    unsigned long rsStart;
    size_t rsLen;                    // data length known so far (without an added EOI)
    bool rsEndSeen;
    uint16_t rsChunks;               // chunks of the data known so far
    uint8_t rsHave[HAVE_BYTES];
    uint8_t rsRounds;
    uint16_t rsReqEnd;               // last chunk of the current request
    unsigned long rsTime;

    // pending publish
    bool pubActive;
    const uint8_t* pubBuf;
    size_t pubLen;
    unsigned long pubSince;
};

// On the heap, allocated once by a static initializer (zeroed like a static array would be): the
// ~2.3 KB of slot state did not fit .dram0.bss. ESP-IDF runs C++ static initializers after the heap
// is set up; a failure there leaves the gateway without pictures, which imageReceiverInit() reports.
static CamPic* const s_cam = static_cast<CamPic*>(calloc(IMG_MAX_CAMERAS, sizeof(CamPic)));
static const int s_nCam = s_cam ? IMG_MAX_CAMERAS : 0;   // every loop over the slots uses this
static uint8_t* s_tmp = nullptr;             // splice output, swapped with a slot's jpg (used within one jpegFinish)
static uint32_t s_seq = 0;                   // picture counter, tells which camera has the newest picture
static uint32_t s_noSlotDrops = 0;           // chunks dropped because every slot was busy

// test (serial command "drop <n>"): drop every n-th 56-byte picture chunk, 0 = off
static uint32_t s_dropEvery = 0, s_dropCount = 0;
static bool s_noResend = false;      // finishing because the next picture started: no resend
static bool s_resendDone = false;    // jpegFinish() completes a picture by resend
void imageReceiverSetDrop(uint32_t n) { s_dropEvery = n; s_dropCount = 0; }

#ifdef HOST_TEST
static int s_hostFailAlloc = -1;     // allocBig() call that fails (0 = the next one), -1 = none
#endif

static uint8_t* allocBig(size_t n) {
#ifdef HOST_TEST
    if (s_hostFailAlloc == 0) {
        s_hostFailAlloc = -1;
        return nullptr;
    }
    if (s_hostFailAlloc > 0) s_hostFailAlloc--;
#endif
    uint8_t* p = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = (uint8_t*)malloc(n);
    return p;
}

static void publishPending(CamPic* c) {
    if (!c->pubActive) return;
    c->pubActive = false;
    if (!mqttPublishCameraImage(c->deviceId, c->pubBuf, c->pubLen))
        Serial.printf("[ImageReceiver] Failed to publish image to MQTT broker.\n");
}

// A picture with holes is held while its resend runs: the camera resends and polls again ~60-80 ms
// after the answer that asked for the chunks, and a blocking publish in between would make that
// poll miss its answer deadline. When the resend completes, the complete picture replaces it (see
// jpegFinish); when it ends without completing (rsCancel), the next imageReceiverLoop() or poll
// hook publishes the first pass.
static bool publishHeld(const CamPic* c) {
    return c->pubActive && c->rsActive;
}

// Mark the picture for publishing. stable: where it is stored (jpg, jpgLarge, jpgGray), or
// nullptr to copy len bytes of imgBuf to the slot's own pubImg (a picture without frame header, or too
// big for the gray buffer; allocated on the first need, rare). Without a buffer for the copy it is
// published at once. Every pending picture has a buffer of its own, so finishing one camera's picture
// never publishes another camera's (a blocking publish that ignores publishHeld() and the poll order).
static void queuePublish(CamPic* c, const uint8_t* stable, size_t len) {
    if (!stable) {
        if (!c->pubImg) c->pubImg = allocBig(MAX_IMAGE_SIZE);
        if (!c->pubImg) {
            if (!mqttPublishCameraImage(c->deviceId, c->imgBuf, len))
                Serial.printf("[ImageReceiver] Failed to publish image to MQTT broker.\n");
            return;
        }
        memcpy(c->pubImg, c->imgBuf, len);
        stable = c->pubImg;
    }
    c->pubBuf = stable;
    c->pubLen = len;
    c->pubSince = millis();
    c->pubActive = true;
}

static void jpegFinish(CamPic* c, size_t len, bool endSeen = true);

static void jpegReset(CamPic* c) {
    if (c->imgBuf) memset(c->imgBuf, 0, MAX_IMAGE_SIZE);
    memset(c->have, 0, sizeof(c->have));
    c->imgEnd = 0;
    c->haveCount = 0;
    c->lastIdx = -1;
    c->imgDirty = false;
}

// Forget everything about the camera in the slot (the buffers stay allocated), ready for a new camera.
static void slotClear(CamPic* c) {
    uint8_t* img = c->imgBuf;
    uint8_t* jpg = c->jpg;
    uint8_t* large = c->jpgLarge;
    uint8_t* gray = c->jpgGray;
    uint8_t* pubImg = c->pubImg;
    if (img && c->used) memset(img, 0, MAX_IMAGE_SIZE);   // (an unused slot's imgBuf is clean already)
    memset(c, 0, sizeof(*c));
    c->imgBuf = img;
    c->jpg = jpg;
    c->jpgLarge = large;
    c->jpgGray = gray;
    c->pubImg = pubImg;
    c->baseId = -1;
    c->lastIdx = -1;
}

static CamPic* findCam(const char* deviceId) {
    for (int i = 0; i < s_nCam; i++)
        if (s_cam[i].used && strncmp(s_cam[i].deviceId, deviceId, 9) == 0) return &s_cam[i];
    return nullptr;
}

// The slot of this camera; on its first chunk a free slot, else the idle slot (no open picture, no
// resend, no pending publish) that has been quiet the longest. nullptr (chunk dropped, counted) if
// every slot is busy.
static CamPic* assignCam(const char* deviceId, unsigned long now) {
    CamPic* c = findCam(deviceId);
    if (c) return c;
    CamPic* pick = nullptr;
    unsigned long oldest = 0;
    for (int i = 0; i < s_nCam; i++) {
        CamPic* s = &s_cam[i];
        if (!s->used) {
            pick = s;
            break;
        }
        bool idle = s->lastIdx < 0 && !s->rsActive && !s->pubActive;
        unsigned long age = now - s->lastUse;
        if (idle && (!pick || age > oldest)) {
            pick = s;
            oldest = age;
        }
    }
    if (!pick) {
        if (s_nCam == 0) return nullptr;     // no slot memory at all (reported by imageReceiverInit)
        if ((++s_noSlotDrops & 0x3F) == 1)
            Serial.printf("[ImageReceiver] No free picture slot for %.9s: chunks dropped (%lu so far)\n", deviceId,
                          (unsigned long)s_noSlotDrops);
        return nullptr;
    }
    if (pick->used)
        Serial.printf("[ImageReceiver] Picture slot of %s given to %.9s\n", pick->deviceId, deviceId);
    slotClear(pick);
    pick->used = true;
    strncpy(pick->deviceId, deviceId, 9);
    pick->deviceId[9] = '\0';
    return pick;
}

// The slot's big buffers (and the shared splice buffer), allocated once. If one fails the ones this
// call got are given back, the failure is logged and the camera's pictures are dropped (the other
// cameras are not affected) until a chunk arrives ALLOC_RETRY_MS later, which tries again.
static bool slotBuffers(CamPic* c) {
    if (c->allocFailed) {
        // retry on a later chunk of this camera (memory may have come free), at most once per ALLOC_RETRY_MS
        if (millis() - c->allocFailTime < ALLOC_RETRY_MS) return false;
        c->allocFailed = false;
    }
    if (c->imgBuf && c->jpg && s_tmp) return true;
    if (!s_tmp) s_tmp = allocBig(MAX_IMAGE_SIZE);
    bool newImg = !c->imgBuf, newJpg = !c->jpg;
    if (newImg) c->imgBuf = allocBig(MAX_IMAGE_SIZE);
    if (newJpg) c->jpg = allocBig(MAX_IMAGE_SIZE);
    if (s_tmp && c->imgBuf && c->jpg) {
        if (newImg) jpegReset(c);
        return true;
    }
    if (newImg && c->imgBuf) free(c->imgBuf);       // only what this call allocated
    if (newJpg && c->jpg) free(c->jpg);
    if (newImg) c->imgBuf = nullptr;
    if (newJpg) c->jpg = nullptr;
    c->allocFailed = true;
    c->allocFailTime = millis();
    Serial.printf("[ImageReceiver] No memory for the picture buffers of %s (%u bytes of PSRAM free): its pictures are dropped\n",
                  c->deviceId, (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return false;
}

// Picture width from the SOF0 marker (0 if not found).
static unsigned int jpegWidth(const uint8_t* j, size_t len) {
    for (size_t i = 2; i + 9 < len && i < 1024; i++)
        if (j[i] == 0xFF && j[i + 1] == 0xC0)
            return ((unsigned int)j[i + 7] << 8) | j[i + 8];
    return 0;
}

static void requestNewBase(CamPic* c) {
    if (c->baseRequested && millis() - c->baseReqTime < BASE_REQUEST_MS) return;
    c->baseRequested = true;
    c->baseReqTime = millis();
    bool ok = downlinkRequestPicture(c->deviceId, 0xD1);
    Serial.printf("[ImageReceiver] No base for this delta: full picture 0xD1 for %s %s\n", c->deviceId,
                  ok ? "queued" : "NOT queued");
}

static bool rsHave(const CamPic* c, uint16_t i) { return (c->rsHave[i >> 3] >> (i & 7)) & 1; }

// End the resend (completed or given up): also drop a request still queued in the downlink, so
// it does not go out later, ahead of a queued command, and ask for chunks of a picture the
// gateway no longer completes.
static void rsCancel(CamPic* c) {
    if (c->rsActive) downlinkCancelResend(c->deviceId);
    c->rsActive = false;
}

// Ask for the missing chunks among the 32 from the first missing one (a bitmap, so chunks the
// gateway already has are not sent again; with the end not seen yet the chunks after the
// known data count as missing), or finish.
static void rsRequestNext(CamPic* c) {
    uint16_t first = 0xFFFF, last = 0, i;
    uint32_t map = 0;
    for (i = 0; i < c->rsChunks; i++) {
        if (!rsHave(c, i)) {
            first = i;
            break;
        }
    }
    if (first != 0xFFFF || !c->rsEndSeen) {
        if (first == 0xFFFF) first = c->rsChunks;   // the end is missing: ask for what follows
        for (i = 0; i < 32 && first + i < JPEG_MAX_CHUNKS; i++) {
            uint16_t ch = first + i;
            if (ch < c->rsChunks ? !rsHave(c, ch) : !c->rsEndSeen) {
                map |= 1UL << i;
                last = ch;
            }
        }
    }
    if (first == 0xFFFF) {               // complete: store and publish it as a normal picture
        Serial.printf("[ImageReceiver] Picture from %s complete after resend: %u bytes, %u round(s), %lu ms\n",
                      c->deviceId, (unsigned int)c->rsLen, c->rsRounds, millis() - c->rsStart);
        rsCancel(c);
        if (c->lastIdx >= 0) return;     // a new picture is coming in (cannot happen: it cancels)
        c->haveCount = c->rsChunks;      // the picture is in imgBuf already
        s_noResend = true;
        s_resendDone = true;
        jpegFinish(c, c->rsLen, true);
        s_resendDone = false;
        s_noResend = false;
        return;
    }
    if (!map) {                          // nothing addressable (picture at JPEG_MAX_CHUNKS)
        rsCancel(c);
        return;
    }
    if (++c->rsRounds > RS_MAX_ROUNDS || millis() - c->rsStart > RS_TOTAL_MS) {
        Serial.printf("[ImageReceiver] Resend for %s stopped after %u rounds\n", c->deviceId, c->rsRounds - 1);
        rsCancel(c);
        // released right after a resend batch: restart the defer time so the camera's next poll
        // (~30-40 ms later) is answered before this blocking publish
        if (c->pubActive) c->pubSince = millis();
        return;
    }
    c->rsReqEnd = last;
    c->rsTime = millis();
    bool ok = downlinkRequestResend(c->deviceId, first, map);
    Serial.printf("[ImageReceiver] Resend of %d chunks in %u..%u from %s %s\n", __builtin_popcount(map), first, last,
                  c->deviceId, ok ? "queued" : "NOT queued");
}

void imageReceiverProcessResend(const char* deviceId, uint16_t chunkIdx, uint8_t chunkLen, bool isLast,
                                const uint8_t* data) {
    if (!deviceId || !data || chunkLen == 0 || chunkLen > JPEG_CHUNK || chunkIdx >= JPEG_MAX_CHUNKS) return;
    CamPic* c = findCam(deviceId);
    if (!c || !c->rsActive) return;
    size_t off = (size_t)chunkIdx * JPEG_CHUNK;
    if (off + chunkLen > MAX_IMAGE_SIZE) return;
    memcpy(c->imgBuf + off, data, chunkLen);
    c->rsHave[chunkIdx >> 3] |= (uint8_t)(1 << (chunkIdx & 7));
    if (isLast) {
        c->rsLen = off + chunkLen;
        c->rsEndSeen = true;
        c->rsChunks = chunkIdx + 1;
    } else if (off + chunkLen > c->rsLen) {
        c->rsLen = off + chunkLen;
        if (chunkIdx + 1 > c->rsChunks) c->rsChunks = chunkIdx + 1;
    }
    c->rsTime = millis();
    if (chunkIdx >= c->rsReqEnd || isLast) rsRequestNext(c);
}

// Finish the blob in imgBuf (len bytes; missing chunks stay zero): a full JPEG (kept as
// the base for deltas if it is 320 wide, or as the large picture) or a delta blob that is
// spliced into the base (jpeg_splice.cpp). The resulting picture is marked for publishing on
// MQTT (queuePublish: published after the camera's poll answer, see CamPic).
// endSeen false: the last chunk never came (the blob ended by a pause or the next blob), so
// the missing count cannot include the lost end; a JPEG gets its EOI back so it decodes.
static void jpegFinish(CamPic* c, size_t len, bool endSeen) {
    if (len < 4 || !c->imgBuf || !c->jpg || !s_tmp) {
        jpegReset(c);
        return;
    }
    // A picture still waiting to be published must go out before the slot's jpg & co are overwritten, or it
    // would be dropped. Exception: the same picture completed by resend replaces its first pass
    // (holes), so only the complete one is published (one blocking publish instead of two).
    if (c->pubActive) {
        if (s_resendDone)
            c->pubActive = false;
        else
            publishPending(c);
    }
    uint8_t* imgBuf = c->imgBuf;
    uint16_t chunks = (len + JPEG_CHUNK - 1) / JPEG_CHUNK;
    uint16_t missing = chunks > c->haveCount ? chunks - c->haveCount : 0;
    const char* endNote = endSeen ? "" : " (end lost)";
    size_t dataLen = len;
    if (!endSeen && !jpegIsDelta(imgBuf, len) && len + 2 <= MAX_IMAGE_SIZE &&
        !(imgBuf[len - 2] == 0xFF && imgBuf[len - 1] == 0xD9)) {
        imgBuf[len++] = 0xFF;
        imgBuf[len++] = 0xD9;
    }
    if (jpegIsDelta(imgBuf, len)) {
        int id = jpegDeltaId(imgBuf, len);
        unsigned int updated = 0;
        size_t n = 0;
        if (c->jpgLen && id == c->baseId)
            n = jpegApplyDelta(c->jpg, c->jpgLen, imgBuf, len, s_tmp, MAX_IMAGE_SIZE, &updated);
        Serial.printf("[ImageReceiver] Delta from %s: %u bytes for picture %d, %u blocks%s, %u/%u chunks missing%s\n",
                      c->deviceId, (unsigned int)len, id, updated,
                      n ? "" : (id == c->baseId ? " (could not apply)" : " (other base, ignored)"), missing, chunks, endNote);
        if (!n)
            requestNewBase(c);
        if (n) {
            uint8_t* t = c->jpg;         // the spliced picture becomes the current one
            c->jpg = s_tmp;
            s_tmp = t;
            c->jpgLen = n;
            c->deltaCount++;
            if (updated) {
                c->grayNewest = false;
                c->seq320 = ++s_seq;
                queuePublish(c, c->jpg, n);
            }
        }
        jpegReset(c);
        return;
    }
    bool soi = imgBuf[0] == 0xFF && imgBuf[1] == 0xD8;
    unsigned int w = jpegWidth(imgBuf, len);
    const uint8_t* stable = nullptr;                // where the picture is kept for the publish
    if (w == 0) {
        // no frame header (lost with a chunk): published, but neither stored as the base
        // nor as the large picture until a resend completes it
    } else if (w > 320) {                           // large (640x480) picture: kept apart, no base
        if (!c->jpgLarge) c->jpgLarge = allocBig(MAX_IMAGE_SIZE);
        if (c->jpgLarge) {
            memcpy(c->jpgLarge, imgBuf, len);
            c->jpgLargeLen = len;
            c->jpgLargeMissing = missing;
            c->seqLarge = ++s_seq;
            stable = c->jpgLarge;
        }
    } else if (c->baseId >= 0 && jpegCommentId(imgBuf, len) < 0) {
        // no COM id (grayscale, e.g. a flash picture 0xD4): published below, but the colour
        // picture stays the base for the camera's next delta
        if (!c->jpgGray) c->jpgGray = allocBig(GRAY_MAX);
        if (c->jpgGray && len <= GRAY_MAX) {
            memcpy(c->jpgGray, imgBuf, len);
            c->jpgGrayLen = len;
            c->jpgGrayMissing = missing;
            c->grayNewest = true;
            c->seq320 = ++s_seq;
            stable = c->jpgGray;
        }
    } else {
        c->grayNewest = false;
        memcpy(c->jpg, imgBuf, len);
        c->jpgLen = len;
        c->jpgMissing = missing;
        c->baseId = jpegCommentId(c->jpg, len);
        c->deltaCount = 0;
        c->seq320 = ++s_seq;
        stable = c->jpg;
    }
    Serial.printf("[ImageReceiver] JPEG from %s: %u bytes, width %u, id %d, %u/%u chunks missing%s%s\n", c->deviceId,
                  (unsigned int)len, w, jpegCommentId(imgBuf, len), missing, chunks, soi ? "" : " (no SOI)", endNote);
    queuePublish(c, stable, len);
    if (!s_noResend && (missing || !endSeen)) {
        // the picture stays in imgBuf (the buffer of the resend); only the open-picture state is cleared
        c->rsActive = true;
        c->rsStart = millis();
        c->rsLen = dataLen;
        c->rsEndSeen = endSeen;
        c->rsChunks = (dataLen + JPEG_CHUNK - 1) / JPEG_CHUNK;
        memcpy(c->rsHave, c->have, sizeof(c->rsHave));
        c->rsRounds = 0;
        c->imgDirty = true;
        c->imgEnd = 0;
        c->haveCount = 0;
        c->lastIdx = -1;
        memset(c->have, 0, sizeof(c->have));
        rsRequestNext(c);
        if (!c->rsActive) jpegReset(c);    // nothing to resend after all
        return;
    }
    jpegReset(c);
}

void imageReceiverPollSeen(const char* deviceId) {
    CamPic* c = deviceId ? findCam(deviceId) : nullptr;
    if (!c) return;
    // the camera polls right after a picture: a picture still open lost its last chunk; finish
    // it now so a resend request can go out with this poll's answer (the Blackfin is still on)
    if (c->lastIdx >= 0) jpegFinish(c, c->imgEnd, false);
    // a resend round whose last chunk was lost never asks for the next one (that happens when
    // the last requested chunk arrives): ask again for what is still missing with this poll's
    // answer. The camera polls every ~2 s until an answer without a resend request.
    if (c->rsActive && !downlinkResendPending(c->deviceId))
        rsRequestNext(c);
}

// Publish every waiting picture, the oldest first.
void imageReceiverPublishPending(void) {
    for (;;) {
        CamPic* o = nullptr;
        for (int i = 0; i < s_nCam; i++)
            if (s_cam[i].pubActive && (!o || (long)(o->pubSince - s_cam[i].pubSince) > 0)) o = &s_cam[i];
        if (!o) return;
        publishPending(o);
    }
}

void imageReceiverAfterPollAnswer(const char* deviceId) {
    CamPic* c = deviceId ? findCam(deviceId) : nullptr;
    if (c && c->pubActive && !publishHeld(c)) publishPending(c);
}

void imageReceiverInit(void) {
    if (!s_cam) {
        Serial.println("[ImageReceiver] ERROR: no memory for the camera slots: pictures are disabled");
        return;
    }
    for (int i = 0; i < s_nCam; i++) {
        if (!s_cam[i].used) continue;
        rsCancel(&s_cam[i]);              // imgBuf is the resend's buffer too
        jpegReset(&s_cam[i]);
    }
    // the buffers (128 KB per camera, +64 KB for a 640 picture, +16 KB for a flash picture, +64 KB for
    // the rare headerless/oversized-gray copy, plus 64 KB splice buffer once) are allocated when needed
    Serial.printf("[ImageReceiver] PSRAM: %u bytes total, %u free; picture buffers are allocated per camera on its first picture (%d cameras)\n",
                  (unsigned int)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
                  (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), IMG_MAX_CAMERAS);
}

void imageReceiverProcessChunk(const char* deviceId, uint16_t chunkIdx, uint8_t chunkLen,
                               bool isFirst, bool isLast, const uint8_t* data) {
    if (!deviceId || !data || chunkLen == 0 || chunkLen > JPEG_CHUNK || chunkIdx >= JPEG_MAX_CHUNKS ||
        (size_t)chunkIdx * JPEG_CHUNK + chunkLen > MAX_IMAGE_SIZE) return;
    unsigned long now = millis();
    CamPic* c = assignCam(deviceId, now);
    if (!c) return;
    c->lastUse = now;
    rsCancel(c);                          // a new picture of this camera: the stored one may change (deltas)
    if (s_dropEvery && ++s_dropCount % s_dropEvery == 0) return;   // test
    if (!slotBuffers(c)) return;
    if (c->imgDirty) jpegReset(c);        // the buffer still holds the picture of the cancelled resend

    bool stale = c->lastIdx >= 0 && now - c->lastChunkTime > 15000;
    bool wrapped = c->lastIdx >= 0 && (int32_t)chunkIdx < c->lastIdx;   // next picture, first chunk lost
    if (stale) {
        jpegReset(c);
    } else if ((isFirst || wrapped) && c->lastIdx >= 0) {
        s_noResend = true;              // the camera's buffer already holds the next picture
        jpegFinish(c, c->imgEnd, false);    // previous picture lost its last chunk
        s_noResend = false;
    }
    c->lastChunkTime = now;

    size_t off = (size_t)chunkIdx * JPEG_CHUNK;
    memcpy(c->imgBuf + off, data, chunkLen);
    uint8_t bit = 1 << (chunkIdx & 7);
    if (!(c->have[chunkIdx >> 3] & bit)) {
        c->have[chunkIdx >> 3] |= bit;
        c->haveCount++;
    }
    if (off + chunkLen > c->imgEnd) c->imgEnd = off + chunkLen;
    c->lastIdx = chunkIdx;

    if (isLast) jpegFinish(c, off + chunkLen);
}

// The camera with the newest 320 picture (or large picture), nullptr if none has one.
static const CamPic* newestCam(bool large) {
    const CamPic* best = nullptr;
    for (int i = 0; i < s_nCam; i++) {
        const CamPic* c = &s_cam[i];
        if (!c->used || !(large ? c->jpgLargeLen : (c->jpgLen || (c->grayNewest && c->jpgGrayLen)))) continue;
        uint32_t seq = large ? c->seqLarge : c->seq320;
        if (!best || (int32_t)(seq - (large ? best->seqLarge : best->seq320)) > 0) best = c;
    }
    return best;
}

const uint8_t* imageReceiverJpeg(size_t* len, uint16_t* missing, const char* deviceId) {
    const CamPic* c = deviceId ? findCam(deviceId) : newestCam(false);
    if (c && c->grayNewest && c->jpgGrayLen) {
        if (len) *len = c->jpgGrayLen;
        if (missing) *missing = c->jpgGrayMissing;
        return c->jpgGray;
    }
    if (len) *len = c ? c->jpgLen : 0;
    if (missing) *missing = c ? c->jpgMissing : 0;
    return c && c->jpgLen ? c->jpg : nullptr;
}

const uint8_t* imageReceiverJpegLarge(size_t* len, uint16_t* missing, const char* deviceId) {
    const CamPic* c = deviceId ? findCam(deviceId) : newestCam(true);
    if (len) *len = c ? c->jpgLargeLen : 0;
    if (missing) *missing = c ? c->jpgLargeMissing : 0;
    return c && c->jpgLargeLen ? c->jpgLarge : nullptr;
}

void imageReceiverLoop(void) {
    for (int i = 0; i < s_nCam; i++) {
        CamPic* c = &s_cam[i];
        if (!c->used) continue;
        // a JPEG/delta whose last chunk was lost: finish it after a pause instead of dropping it
        // when the next picture comes more than 15 s later (stale)
        if (c->lastIdx >= 0 && millis() - c->lastChunkTime > JPEG_QUIET_MS) jpegFinish(c, c->imgEnd, false);
        // millis() again: jpegFinish() above may just have started a resend (rsTime > now), and
        // now - rsTime would wrap and give it up at once (seen when a picture's end was lost)
        if (c->rsActive && millis() - c->rsTime > RS_TIMEOUT_MS) {
            Serial.printf("[ImageReceiver] Resend for %s given up (no answer)\n", c->deviceId);
            rsCancel(c);
        }
        // a picture no poll followed (or whose poll was not seen): publish it after the defer time
        if (c->pubActive && millis() - c->pubSince >= IMG_PUBLISH_DEFER_MS && !publishHeld(c)) publishPending(c);
    }
}

#ifdef HOST_TEST
/* Host unit tests only (868Receiver/test_host), never in the firmware: forget the pictures and the
 * reassembly and resend state as a gateway restart does (the buffers stay allocated). */
void imageReceiverHostTestRestart() {
    for (int i = 0; i < s_nCam; i++) slotClear(&s_cam[i]);
    s_resendDone = false;
    s_dropEvery = s_dropCount = 0;
    s_noResend = false;
    s_noSlotDrops = 0;
    s_hostFailAlloc = -1;
}

/* Give every buffer back, so that the next pictures allocate again (allocation failure tests). */
void imageReceiverHostTestFreeBuffers() {
    imageReceiverHostTestRestart();
    for (int i = 0; i < s_nCam; i++) {
        free(s_cam[i].imgBuf);
        free(s_cam[i].jpg);
        free(s_cam[i].jpgLarge);
        free(s_cam[i].jpgGray);
        free(s_cam[i].pubImg);
        s_cam[i].imgBuf = s_cam[i].jpg = s_cam[i].jpgLarge = s_cam[i].jpgGray = s_cam[i].pubImg = nullptr;
    }
    free(s_tmp);
    s_tmp = nullptr;
}

/* The n-th next big allocation fails (0 = the next one); slot buffers are allocated in the order
 * splice buffer (once), imgBuf, jpg, then jpgLarge / jpgGray when first needed. */
void imageReceiverHostTestFailAlloc(int n) { s_hostFailAlloc = n; }

/* Chunks dropped since the restart because every slot was busy. */
uint32_t imageReceiverHostTestNoSlotDrops() { return s_noSlotDrops; }
#endif
