/**
 * @file crypto.cpp
 * @brief Implementation of AES-128 Decryption, KDF and Replay Protection
 */

#include "crypto.h"
#include "mbedtls/aes.h"
#include "image_receiver.h"
#include "gateway_diag.h"
#include <ctype.h>
#include <string.h>
#include <Preferences.h>

/** @brief Maximum number of distinct sensor nodes tracked simultaneously in RAM. */
#define MAX_TRACKED_DEVICES 32

/**
 * @struct DeviceSession
 * @brief Internal session tracking state for replay attack protection.
 */
struct DeviceSession {
    char deviceId[9];       /**< 8-character device ID */
    uint16_t lastSeq;       /**< Last accepted 16-bit sequence counter */
    bool hasLastSeq;        /**< True if at least one packet has been validated */
    bool ctrMode;           /**< Device sends a 32-bit counter that survives its resets (flag 0x20) */
    uint32_t lastCtr;       /**< Last accepted 32-bit counter (ctrMode) */
    bool needBoot;          /**< After 'replay reset': accept only a boot packet (low byte 0) */
    uint32_t resetMaxCtr;   /**< needBoot: the highest boot packet counter taken (block limit) */
    unsigned long lastStore;/**< millis() of the last NVS write of lastCtr */
    bool storePending;      /**< lastCtr is newer than NVS (written by cryptoReplayFlush) */
    uint32_t picTag;        /**< Tag (start packet counter) of the newest accepted picture chunk */
    bool hasPicTag;         /**< picTag is valid (a picture chunk was accepted since the session began) */
    bool picTagRetired;     /**< True once picTag finished (including any resend) to reject 0x82/0x83 replay */
    bool picTagResendOnly;  /**< True while picTag is in resend: reject 0x82 replay, allow 0x83 */
    unsigned long lastSeen; /**< Timestamp (millis) when last authenticated packet arrived */
};

static DeviceSession s_devices[MAX_TRACKED_DEVICES];
static size_t s_deviceCount = 0;

bool cryptoDeriveDeviceKey(const uint8_t masterKey[16], const char* deviceId, uint8_t outDeviceKey[16]) {
    if (!masterKey || !deviceId || !outDeviceKey) return false;

    // Pad 8-character device ID with zeros to form a 16-byte block
    uint8_t paddedId[16] = {0};
    size_t len = strlen(deviceId);
    if (len > 8) len = 8;
    memcpy(paddedId, deviceId, len);

    // Device_Key = AES-128-ECB(Master_Key, Pad16(Device_ID))
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_enc(&aes, masterKey, 128) != 0) {
        mbedtls_aes_free(&aes);
        return false;
    }
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, paddedId, outDeviceKey);
    mbedtls_aes_free(&aes);
    return true;
}

void cryptoKeyCheckValue(const uint8_t key[16], char out[7]) {
    static const char hex[] = "0123456789ABCDEF";
    uint8_t zero[16] = {0}, enc[16];
    strcpy(out, "------");
    if (!key || !cryptoEncryptPayload(key, zero, enc)) return;
    for (int i = 0; i < 3; i++) {
        out[2 * i] = hex[enc[i] >> 4];
        out[2 * i + 1] = hex[enc[i] & 0x0F];
    }
    out[6] = '\0';
}

bool cryptoEncryptPayload(const uint8_t deviceKey[16], const uint8_t inPlain[16], uint8_t outCipher[16]) {
    if (!deviceKey || !inPlain || !outCipher) return false;

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_enc(&aes, deviceKey, 128) != 0) {
        mbedtls_aes_free(&aes);
        return false;
    }
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, inPlain, outCipher);
    mbedtls_aes_free(&aes);
    return true;
}

bool cryptoDecryptPayload(const uint8_t deviceKey[16], const uint8_t inCipher[16], uint8_t outPlain[16]) {
    if (!deviceKey || !inCipher || !outPlain) return false;

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_dec(&aes, deviceKey, 128) != 0) {
        mbedtls_aes_free(&aes);
        return false;
    }
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, inCipher, outPlain);
    mbedtls_aes_free(&aes);
    return true;
}

/* ---- Persistent replay floor (NVS) ----------------------------------------------------------
 * Devices that set flag 0x20 send a 32-bit counter (seq in plain[0..1], high half in plain[14..15])
 * whose upper 24 bits (the "block") the device keeps in flash, so it never goes back, not even
 * after a reset. The gateway keeps the last accepted counter in NVS, written for every accepted
 * packet but at most every 2 s (a door sensor sends a few packets an hour, the camera a poll
 * every 30 s and every 2 s after a picture), so after a gateway restart only packets newer than
 * that are accepted. A packet that comes within 2 s of the last write is written by
 * cryptoReplayFlush() (called from loop()) once the 2 s are over, so only packets accepted less
 * than ~2 s before a restart can be replayed. */
#define CTR_STORE_MIN_MS 2000
static Preferences s_replayPrefs;
static bool s_replayPrefsOpen = false;

static bool replayPrefsOpen() {
    if (!s_replayPrefsOpen) s_replayPrefsOpen = s_replayPrefs.begin("replay", false);
    return s_replayPrefsOpen;
}

static bool ctrLoad(const char* devId, uint32_t* floor) {
    if (!replayPrefsOpen() || !s_replayPrefs.isKey(devId)) return false;
    *floor = s_replayPrefs.getULong(devId, 0);
    return true;
}

static void ctrStore(const char* devId, uint32_t ctr) {
    if (replayPrefsOpen()) s_replayPrefs.putULong(devId, ctr);
}

/* After 'replay reset' the device waits for a boot packet; that state is kept in NVS under
 * "R<ID>" (the highest counter taken), so a gateway restart in between does not leave the device
 * with no floor at all (then any recorded packet would pass). Removed with the first boot packet. */
static void resetKey(const char* devId, char key[10]) {
    key[0] = 'R';
    memcpy(key + 1, devId, 8);
    key[9] = '\0';
}

static bool resetLoad(const char* devId, uint32_t* maxCtr) {
    char k[10];
    resetKey(devId, k);
    if (!replayPrefsOpen() || !s_replayPrefs.isKey(k)) return false;
    *maxCtr = s_replayPrefs.getULong(k, 0);
    return true;
}

static void resetStore(const char* devId, uint32_t maxCtr, bool on) {
    char k[10];
    resetKey(devId, k);
    if (!replayPrefsOpen()) return;
    if (on) s_replayPrefs.putULong(k, maxCtr);
    else if (s_replayPrefs.isKey(k)) s_replayPrefs.remove(k);
}

/**
 * @brief Find an existing session or allocate a new session record for a device.
 * @param deviceId Sensor node identifier.
 * @return Pointer to DeviceSession or nullptr if table is full.
 */
static DeviceSession* getOrCreateSession(const char* deviceId, bool onlyKnown = false) {
    for (size_t i = 0; i < s_deviceCount; i++) {
        if (strcmp(s_devices[i].deviceId, deviceId) == 0) {
            return &s_devices[i];
        }
    }
    // onlyKnown: create a session only for a device with a replay floor in NVS. The picture chunk
    // path has no real authentication before this point (a random frame passes its type and
    // length check about once in 200), so it must not fill the table with made-up IDs.
    uint32_t known;
    if (onlyKnown && !ctrLoad(deviceId, &known) && !resetLoad(deviceId, &known)) return nullptr;
    if (s_deviceCount < MAX_TRACKED_DEVICES) {
        DeviceSession* s = &s_devices[s_deviceCount++];
        strncpy(s->deviceId, deviceId, 8);
        s->deviceId[8] = '\0';
        s->lastSeq = 0;
        s->hasLastSeq = false;
        s->ctrMode = false;
        s->lastCtr = 0;
        s->needBoot = false;
        s->resetMaxCtr = 0;
        s->lastStore = 0;
        s->storePending = false;
        s->picTag = 0;
        s->hasPicTag = false;
        s->picTagRetired = false;
        s->picTagResendOnly = false;
        s->lastSeen = 0;
        // A device that has sent the persistent counter keeps its floor across gateway restarts
        uint32_t floor;
        if (ctrLoad(s->deviceId, &floor)) {
            s->ctrMode = true;
            s->lastCtr = floor;
            s->hasLastSeq = true;
        } else if (resetLoad(s->deviceId, &floor)) {   // 'replay reset' still waiting
            s->ctrMode = true;
            s->hasLastSeq = true;
            s->needBoot = true;
            s->resetMaxCtr = floor;
        }
        return s;
    }
    g_gatewayDiag.sessionFull++;
    return nullptr; // Session table full
}

// 'replay reset' is for a device reflashed without a counter seed (--no-seed): it starts at block
// 1 and moves 2 blocks per boot (a camera also 1 block per 256 packets, ~2 h of polls), so by
// default only a boot packet of blocks 1..8 is taken: any recorded packet from before has a
// higher block. 'replay reset <ID> <block>' raises the limit for a device that is further on.
#define REPLAY_RESET_DEFAULT_BLOCK 8

#define PIC_TAG_AHEAD  64      // a picture's start packet may be this far ahead of the last counter
#define PIC_TAG_BEHIND 128      // ... or this far behind (a picture's polls and resend rounds: <~100)

bool cryptoProcessBigPacket(const uint8_t* rxBuf56, const uint8_t masterKey[16], uint8_t channel) {
    if (!rxBuf56 || !masterKey) return false;
    char devId[9] = {0};
    for (int i = 0; i < 8; i++) {
        devId[i] = (char)rxBuf56[i];
        if (!isalnum((unsigned char)devId[i])) return false;
    }
    uint8_t devKey[16], plain[48];
    if (!cryptoDeriveDeviceKey(masterKey, devId, devKey)) return false;
    for (int b = 0; b < 3; b++) {
        if (!cryptoDecryptPayload(devKey, &rxBuf56[8 + 16 * b], &plain[16 * b])) {
            memset(devKey, 0, sizeof(devKey));
            return false;
        }
        if (b > 0) {
            for (int i = 0; i < 16; i++) plain[16 * b + i] ^= rxBuf56[8 + 16 * (b - 1) + i];
        }
    }
    memset(devKey, 0, sizeof(devKey));
    uint8_t len = plain[5] & 0x3F;
    if ((plain[2] != 0x82 && plain[2] != 0x83) || len == 0 || len > 40) {
        g_gatewayDiag.authFail++;       // wrong key, or not a picture chunk
        gatewayDiagNoteRejected(devId);
        return false;
    }
    g_gatewayDiag.authOk++;
    // Replay check: every chunk carries the 32-bit counter of its picture's start packet (0xB2),
    // low 16 bits in [0..1], high 16 bits in [46..47] (where 'VS' used to be; it is also the
    // integrity check of the last CBC block). The full value is rebuilt from the device's last
    // accepted counter and must lie within PIC_TAG_BEHIND before it (the polls and resend rounds
    // that follow a picture) or PIC_TAG_AHEAD after it (start packet lost), so a recorded picture
    // is useless once the camera has sent ~128 more packets. Within that window a new chunk (0x82)
    // must belong to the newest picture seen (while still open/in resend) or a newer one, and a
    // resent chunk (0x83) to exactly that active picture, so an older or already completed picture
    // and its resent chunks cannot come back. Camera data never moves the counter's replay window
    // itself.
    DeviceSession* sess = getOrCreateSession(devId, true);
    if (!sess || !sess->ctrMode || !sess->hasLastSeq || sess->needBoot) return false;
    uint16_t num = ((uint16_t)plain[0] << 8) | plain[1];
    int16_t d = (int16_t)(num - (uint16_t)sess->lastCtr);
    uint32_t tag = sess->lastCtr + (int32_t)d;
    bool resent = plain[2] == 0x83;
    if (plain[46] != (uint8_t)(tag >> 24) || plain[47] != (uint8_t)(tag >> 16) ||
        d > PIC_TAG_AHEAD || d < -PIC_TAG_BEHIND ||
        (resent ? !sess->hasPicTag || sess->picTagRetired || tag != sess->picTag
                : sess->hasPicTag && ((int32_t)(tag - sess->picTag) < 0 ||
                                      ((sess->picTagRetired || sess->picTagResendOnly) && tag == sess->picTag)))) {
        g_gatewayDiag.picStale++;
        static unsigned long s_lastLog = 0;
        if (millis() - s_lastLog > 5000) {
            s_lastLog = millis();
            Serial.printf("[Replay] Dropped picture chunks from \"%s\": picture %04X%04X, last counter %08lX\n",
                          devId, ((unsigned)plain[46] << 8) | plain[47], (unsigned)num,
                          (unsigned long)sess->lastCtr);
        }
        return true;            // a valid but stale chunk: handled (dropped), not a 24-byte packet
    }
    if (!sess->hasPicTag || (int32_t)(tag - sess->picTag) > 0) {
        sess->picTag = tag;
        sess->hasPicTag = true;
        sess->picTagRetired = false;
        sess->picTagResendOnly = false;
    }
    sess->lastSeen = millis();
    uint16_t idx = ((uint16_t)plain[3] << 8) | plain[4];
    if (resent)       // a chunk resent on request: patched into the last picture
        imageReceiverProcessResend(devId, idx, len, (plain[5] & 0x40) != 0, &plain[6], channel);
    else
        imageReceiverProcessChunk(devId, idx, len, (plain[5] & 0x80) != 0, (plain[5] & 0x40) != 0, &plain[6], channel, tag);
    return true;
}

void cryptoRetirePicTag(const char* devId, uint32_t tag) {
    if (!devId) return;
    for (size_t i = 0; i < s_deviceCount; i++) {
        if (strcmp(s_devices[i].deviceId, devId) == 0) {
            if (s_devices[i].hasPicTag && s_devices[i].picTag == tag) {
                s_devices[i].picTagRetired = true;
                s_devices[i].picTagResendOnly = false;
            }
            return;
        }
    }
}

void cryptoSetPicTagResendOnly(const char* devId) {
    if (!devId) return;
    for (size_t i = 0; i < s_deviceCount; i++) {
        if (strcmp(s_devices[i].deviceId, devId) == 0) {
            if (s_devices[i].hasPicTag && !s_devices[i].picTagRetired) {
                s_devices[i].picTagResendOnly = true;
            }
            return;
        }
    }
}

/** An authenticated packet dropped by the replay filter: count it, remember the ID. */
static bool replayDrop(const char* devId) {
    g_gatewayDiag.replayDrop++;
    gatewayDiagNoteRejected(devId);
    return false;
}

bool cryptoProcessPacket(const uint8_t* rxBuf24, int rssi, uint8_t lqi, const uint8_t masterKey[16], VerisureTelemetry* outTelem) {
    if (!rxBuf24 || !outTelem || !masterKey) return false;

    // 1. Extract 8-character plaintext Device ID
    char devId[9] = {0};
    for (int i = 0; i < 8; i++) {
        char c = (char)rxBuf24[i];
        devId[i] = (c >= 32 && c <= 126) ? c : '?';
    }
    devId[8] = '\0';
    // IDs end up unescaped in MQTT topics and HA discovery JSON: alphanumeric only
    for (int i = 0; i < 8; i++) {
        if (!isalnum((unsigned char)devId[i])) {
            g_gatewayDiag.authFail++;
            return false;
        }
    }

    // 2. Derive unique Device Key from Master Key
    uint8_t devKey[16];
    if (!cryptoDeriveDeviceKey(masterKey, devId, devKey)) {
        return false;
    }

    // 3. Decrypt 16-byte payload (Bytes 8..23)
    uint8_t plain[16] = {0};
    if (!cryptoDecryptPayload(devKey, &rxBuf24[8], plain)) {
        return false;
    }

    // (Legacy 24-byte picture chunks, type 0x80, are no longer accepted: they carried no counter.
    // A downlink answer echoed back cannot pass below: its [6] is 0 or 1, never a device ID.)

    // 4. Authenticate Magic Bytes (plain[10..11] == 'V', 'S')
    // If the key is wrong or data was corrupted, AES diffusion produces random bytes
    if (plain[10] != 'V' || plain[11] != 'S') {
        g_gatewayDiag.authFail++;
        gatewayDiagNoteRejected(devId);
        return false;
    }

    // 5. Authenticate Device ID Echo (plain[6..9] == devId[0..3])
    // Prevents replay of a ciphertext from a different sensor with a modified header
    if (memcmp(&plain[6], devId, 4) != 0) {
        g_gatewayDiag.authFail++;
        gatewayDiagNoteRejected(devId);
        return false;
    }
    g_gatewayDiag.authOk++;

    // 6. Parse telemetry fields from plaintext
    uint16_t seq = ((uint16_t)plain[0] << 8) | plain[1];
    uint8_t flags = plain[2];
    bool isOp   = (flags & 0x01) != 0;
    bool isShk  = (flags & 0x02) != 0;
    bool isBtn  = (flags & 0x04) != 0;
    bool isPor  = (flags & 0x08) != 0;
    bool isLow  = (flags & 0x10) != 0;
    uint8_t bPct = plain[3];
    uint16_t bMv = ((uint16_t)plain[4] << 8) | plain[5];

    // 7. Replay Attack Protection
    bool hasCtr = (flags & 0x20) != 0;
    uint32_t ctr = ((uint32_t)plain[15] << 24) | ((uint32_t)plain[14] << 16) | seq;
    DeviceSession* sess = getOrCreateSession(devId);
    if (!sess) {
        Serial.printf("[Replay] Dropped \"%s\": session table full\n", devId);
        return false;   // no replay state to check against
    }
    {
        if (hasCtr) {
            // persistent counter: strictly increasing, no exceptions (not for a reset or tamper)
            if (sess->hasLastSeq && sess->ctrMode && ctr <= sess->lastCtr) {
                Serial.printf("[Replay] Dropped \"%s\": counter %lu <= last %lu "
                              "(reflashed sensor? 'replay reset %s')\n",
                              devId, (unsigned long)ctr, (unsigned long)sess->lastCtr, devId);
                return replayDrop(devId);
            }
            // after 'replay reset' (the sensor was reflashed unseeded, its counter restarted at
            // block 1) only a boot packet of one of its first blocks is taken: any recorded boot
            // or block-start packet from before has a higher block and would lock the sensor out
            if (sess->needBoot && ((ctr & 0xFF) != 0 || ctr > sess->resetMaxCtr)) {
                Serial.printf("[Replay] Dropped \"%s\": counter %lu; after 'replay reset' only a boot packet of "
                              "blocks 1..%lu is taken (power-cycle the device, or 'replay reset %s <block>')\n",
                              devId, (unsigned long)ctr, (unsigned long)(sess->resetMaxCtr >> 8), devId);
                return replayDrop(devId);
            }
            if (sess->needBoot) resetStore(devId, 0, false);
            bool store = !sess->ctrMode || sess->needBoot || (ctr >> 8) != (sess->lastCtr >> 8) ||
                         millis() - sess->lastStore >= CTR_STORE_MIN_MS;
            sess->ctrMode = true;
            sess->needBoot = false;
            sess->lastCtr = ctr;
            if (store) {
                ctrStore(devId, ctr);
                sess->lastStore = millis();
            }
            sess->storePending = !store;   // written by cryptoReplayFlush() once 2 s are over
        } else {
            // a device that has sent the persistent counter never falls back to the old format
            // (a recorded old-format packet would otherwise pass with its reset flag)
            if (sess->ctrMode || sess->needBoot) {
                Serial.printf("[Replay] Dropped \"%s\": packet without the persistent counter\n", devId);
                return replayDrop(devId);
            }
            // Old format (camera, older door firmware): 16-bit seq that restarts at 0 after a
            // reset, so the power-on flag must let it through. Tamper gets no exception.
            if (sess->hasLastSeq && !isPor) {
                // Handle 16-bit modular sequence wraparound:
                // Reject duplicate or stale packets (diff <= 0)
                int32_t diff = (int32_t)seq - (int32_t)sess->lastSeq;
                if (diff <= 0 && diff > -32768) {
                    return replayDrop(devId); // Stale or replayed packet!
                }
            }
        }
        sess->lastSeq = seq;
        sess->hasLastSeq = true;
        sess->lastSeen = millis();
    }

    // 8. Populate telemetry result
    strncpy(outTelem->deviceId, devId, 9);
    outTelem->sequence   = seq;
    outTelem->isOpen     = isOp;
    outTelem->isShock    = isShk;
    outTelem->isButton   = isBtn;
    outTelem->isPowerOn  = isPor;
    outTelem->isLowBatt  = isLow;
    outTelem->batteryPct = bPct;
    outTelem->batteryMv  = bMv;
    outTelem->batteryLoadMv = 0;            // camera polls: filled in by the caller
    outTelem->rawP0     = plain[12];
    outTelem->rawP1      = plain[13];
    outTelem->rawP2      = hasCtr ? 0 : plain[14];
    outTelem->rawRst     = hasCtr ? (flags >> 6) : plain[15];   // with the counter: reset cause in flags[7:6]
    outTelem->counter    = hasCtr ? ctr : seq;
    outTelem->hasCounter = hasCtr;
    outTelem->tempC      = 0.0f;
    outTelem->hasTemp    = false;
    outTelem->rssi       = rssi;
    outTelem->lqi           = lqi;
    outTelem->valid         = true;
    outTelem->fastWakeState = 0;
    outTelem->isSmoke       = false;        // set by smokeApply() from rawP0
    outTelem->smokeStatus   = 0;
    outTelem->humidityPct   = 0;
    outTelem->hasHumidity   = false;

    return true;
}

void cryptoReplayFlush() {
    for (size_t i = 0; i < s_deviceCount; i++) {
        DeviceSession* s = &s_devices[i];
        if (s->storePending && millis() - s->lastStore >= CTR_STORE_MIN_MS) {
            ctrStore(s->deviceId, s->lastCtr);
            s->lastStore = millis();
            s->storePending = false;
        }
    }
}

bool cryptoReplayFloor(const char* deviceId, uint32_t* ctr) {
    for (size_t i = 0; i < s_deviceCount; i++) {
        if (strcmp(s_devices[i].deviceId, deviceId) == 0 && s_devices[i].ctrMode) {
            *ctr = s_devices[i].lastCtr;
            return true;
        }
    }
    return ctrLoad(deviceId, ctr);
}

bool cryptoReplayReset(const char* deviceId, uint32_t maxBlock) {
    bool found = false;
    if (strlen(deviceId) != 8) return false;
    if (maxBlock == 0) maxBlock = REPLAY_RESET_DEFAULT_BLOCK;
    if (maxBlock > 0xFEFFFF) maxBlock = 0xFEFFFF;
    for (size_t i = 0; i < s_deviceCount; i++) {
        if (strcmp(s_devices[i].deviceId, deviceId) == 0) found = true;
    }
    if (replayPrefsOpen() && s_replayPrefs.isKey(deviceId)) {
        s_replayPrefs.remove(deviceId);
        found = true;
    }
    // The next packet must be a boot packet in the counter format from block 1..maxBlock (see
    // cryptoProcessPacket); kept in NVS too, so a gateway restart does not open the device.
    uint32_t maxCtr = maxBlock << 8;
    resetStore(deviceId, maxCtr, true);
    DeviceSession* s = getOrCreateSession(deviceId);
    if (s) {
        s->hasLastSeq = true;
        s->ctrMode = true;
        s->lastCtr = 0;
        s->needBoot = true;
        s->resetMaxCtr = maxCtr;
        s->storePending = false;
        s->hasPicTag = false;   // the newest picture tag belongs to the old counter
        s->picTagRetired = false;
        s->picTagResendOnly = false;
    }
    return found;
}

#ifdef HOST_TEST
/* Host unit tests only (868Receiver/test_host), never in the firmware: forget the RAM state as a
 * gateway restart does. NVS (the Preferences stub) keeps what was written. */
void cryptoHostTestRestart() {
    memset(s_devices, 0, sizeof(s_devices));
    s_deviceCount = 0;
    if (s_replayPrefsOpen) s_replayPrefs.end();
    s_replayPrefsOpen = false;
}

bool cryptoHostTestPicTagRetired(const char* devId) {
    if (!devId) return false;
    for (size_t i = 0; i < s_deviceCount; i++) {
        if (strcmp(s_devices[i].deviceId, devId) == 0) {
            return s_devices[i].hasPicTag && s_devices[i].picTagRetired;
        }
    }
    return false;
}

bool cryptoHostTestPicTagResendOnly(const char* devId) {
    if (!devId) return false;
    for (size_t i = 0; i < s_deviceCount; i++) {
        if (strcmp(s_devices[i].deviceId, devId) == 0) {
            return s_devices[i].hasPicTag && s_devices[i].picTagResendOnly;
        }
    }
    return false;
}
#endif
