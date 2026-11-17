/**
 * @file crypto.cpp
 * @brief Implementation of AES-128 Decryption, KDF and Replay Protection
 */

#include "crypto.h"
#include "mbedtls/aes.h"
#include "image_receiver.h"
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
    unsigned long lastStore;/**< millis() of the last NVS write of lastCtr */
    unsigned long lastSeen; /**< Timestamp (millis) when last authenticated packet arrived */
};

static DeviceSession s_devices[MAX_TRACKED_DEVICES];
static bool s_lastWasImage = false;

bool cryptoLastPacketWasImage(void) { return s_lastWasImage; }
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
 * packet but at most every 2 s (a door sensor sends a few packets an hour; the limit only matters
 * if one floods), so after a gateway restart only packets newer than that are accepted. Only
 * packets accepted less than 2 s before the restart can be replayed. */
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

/**
 * @brief Find an existing session or allocate a new session record for a device.
 * @param deviceId Sensor node identifier.
 * @return Pointer to DeviceSession or nullptr if table is full.
 */
static DeviceSession* getOrCreateSession(const char* deviceId) {
    for (size_t i = 0; i < s_deviceCount; i++) {
        if (strcmp(s_devices[i].deviceId, deviceId) == 0) {
            return &s_devices[i];
        }
    }
    if (s_deviceCount < MAX_TRACKED_DEVICES) {
        DeviceSession* s = &s_devices[s_deviceCount++];
        strncpy(s->deviceId, deviceId, 8);
        s->deviceId[8] = '\0';
        s->lastSeq = 0;
        s->hasLastSeq = false;
        s->ctrMode = false;
        s->lastCtr = 0;
        s->needBoot = false;
        s->lastStore = 0;
        s->lastSeen = 0;
        // A device that has sent the persistent counter keeps its floor across gateway restarts
        uint32_t floor;
        if (ctrLoad(s->deviceId, &floor)) {
            s->ctrMode = true;
            s->lastCtr = floor;
            s->hasLastSeq = true;
        }
        return s;
    }
    return nullptr; // Session table full
}

bool cryptoProcessBigPacket(const uint8_t* rxBuf56, const uint8_t masterKey[16]) {
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
    if ((plain[2] != 0x82 && plain[2] != 0x83) || plain[46] != 'V' || plain[47] != 'S' || len == 0 || len > 40)
        return false;
    // camera data never moves the replay window (as the 0x80 chunks)
    DeviceSession* sess = getOrCreateSession(devId);
    if (sess) sess->lastSeen = millis();
    uint16_t idx = ((uint16_t)plain[3] << 8) | plain[4];
    if (plain[2] == 0x83)       // a chunk resent on request: patched into the last picture
        imageReceiverProcessResend(devId, idx, len, (plain[5] & 0x40) != 0, &plain[6]);
    else
        imageReceiverProcessChunk(devId, idx, len, (plain[5] & 0x80) != 0, (plain[5] & 0x40) != 0, &plain[6], 40);
    return true;
}

bool cryptoProcessPacket(const uint8_t* rxBuf24, int rssi, uint8_t lqi, const uint8_t masterKey[16], VerisureTelemetry* outTelem) {
    s_lastWasImage = false;
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
        if (!isalnum((unsigned char)devId[i])) return false;
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

    // 3a. Our own downlink answers ('D','L', seq, chal, armed, cmd, id[0..3], 0, 0, 'V','S';
    // see downlink.h) are never uplink: a replayed one with seq_hi 0x80/0x81 would otherwise
    // pass as an image packet below. The full layout is matched so that a real image
    // packet whose seq happens to be 0x444C ('DL') is not dropped.
    if (plain[0] == 'D' && plain[1] == 'L' &&
        memcmp(&plain[8], devId, 4) == 0 && plain[14] == 'V' && plain[15] == 'S') {
        return false;
    }

    // 3b. Check for RF Camera Image Chunk (plain[2] == 0x80, plain[14..15] == 'V', 'S')
    if (plain[2] == 0x80 && plain[14] == 'V' && plain[15] == 'S') {
        uint16_t seq = ((uint16_t)plain[0] << 8) | plain[1];
        uint16_t chunkIdx = ((uint16_t)plain[3] << 8) | plain[4];
        uint8_t chunkFlags = plain[5];
        uint8_t chunkLen = chunkFlags & 0x0F;
        bool isFirst = (chunkFlags & 0x80) != 0;
        bool isLast  = (chunkFlags & 0x40) != 0;

        // Camera data never moves the replay window: a replayed old chunk would
        // otherwise rewind lastSeq and let old telemetry through again.
        (void)seq;
        DeviceSession* sess = getOrCreateSession(devId);
        if (sess) sess->lastSeen = millis();

        imageReceiverProcessChunk(devId, chunkIdx, chunkLen, isFirst, isLast, &plain[6], 8);
        s_lastWasImage = true;
        return false; // Handled directly as image stream, not a telemetry packet
    }

    // 4. Authenticate Magic Bytes (plain[10..11] == 'V', 'S')
    // If the key is wrong or data was corrupted, AES diffusion produces random bytes
    if (plain[10] != 'V' || plain[11] != 'S') {
        return false;
    }

    // 5. Authenticate Device ID Echo (plain[6..9] == devId[0..3])
    // Prevents replay of a ciphertext from a different sensor with a modified header
    if (memcmp(&plain[6], devId, 4) != 0) {
        return false;
    }

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
                return false;
            }
            // after 'replay reset' (the sensor was reflashed, its counter restarted) only a boot
            // packet is taken, not any recorded packet with a higher counter from before
            if (sess->needBoot && (ctr & 0xFF) != 0) {
                Serial.printf("[Replay] Dropped \"%s\": waiting for a boot packet after 'replay reset'\n", devId);
                return false;
            }
            bool store = !sess->ctrMode || sess->needBoot || (ctr >> 8) != (sess->lastCtr >> 8) ||
                         millis() - sess->lastStore >= CTR_STORE_MIN_MS;
            sess->ctrMode = true;
            sess->needBoot = false;
            sess->lastCtr = ctr;
            if (store) {
                ctrStore(devId, ctr);
                sess->lastStore = millis();
            }
        } else {
            // a device that has sent the persistent counter never falls back to the old format
            // (a recorded old-format packet would otherwise pass with its reset flag)
            if (sess->ctrMode || sess->needBoot) {
                Serial.printf("[Replay] Dropped \"%s\": packet without the persistent counter\n", devId);
                return false;
            }
            // Old format (camera, older door firmware): 16-bit seq that restarts at 0 after a
            // reset, so the power-on flag must let it through. Tamper gets no exception.
            if (sess->hasLastSeq && !isPor) {
                // Handle 16-bit modular sequence wraparound:
                // Reject duplicate or stale packets (diff <= 0)
                int32_t diff = (int32_t)seq - (int32_t)sess->lastSeq;
                if (diff <= 0 && diff > -32768) {
                    return false; // Stale or replayed packet!
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
    outTelem->rawP0      = plain[12];
    outTelem->rawP1      = plain[13];
    outTelem->rawP2      = hasCtr ? 0 : plain[14];
    outTelem->rawRst     = hasCtr ? (flags >> 6) : plain[15];   // with the counter: reset cause in flags[7:6]
    outTelem->counter    = hasCtr ? ctr : seq;
    outTelem->rssi       = rssi;
    outTelem->lqi        = lqi;
    outTelem->valid      = true;

    return true;
}

bool cryptoReplayReset(const char* deviceId) {
    bool found = false;
    if (strlen(deviceId) != 8) return false;
    for (size_t i = 0; i < s_deviceCount; i++) {
        if (strcmp(s_devices[i].deviceId, deviceId) == 0) found = true;
    }
    if (replayPrefsOpen() && s_replayPrefs.isKey(deviceId)) {
        s_replayPrefs.remove(deviceId);
        found = true;
    }
    // The next packet must be a boot packet in the counter format (see cryptoProcessPacket).
    // Kept in RAM only: if the gateway restarts before it arrives, the device starts afresh.
    DeviceSession* s = getOrCreateSession(deviceId);
    if (s) {
        s->hasLastSeq = true;
        s->ctrMode = true;
        s->lastCtr = 0;
        s->needBoot = true;
    }
    return found;
}
