/**
 * @file crypto.cpp
 * @brief Implementation of AES-128 Decryption, KDF and Replay Protection
 */

#include "crypto.h"
#include "mbedtls/aes.h"
#include "image_receiver.h"
#include <ctype.h>
#include <string.h>

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
        s->lastSeen = 0;
        return s;
    }
    return nullptr; // Session table full
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
    if (plain[0] == 'D' && plain[1] == 'L' && plain[12] == 0 && plain[13] == 0 &&
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

        imageReceiverProcessChunk(devId, chunkIdx, chunkLen, isFirst, isLast, &plain[6]);
        s_lastWasImage = true;
        return false; // Handled directly as image stream, not a telemetry packet
    }

    // 3c. Raw grayscale camera tile (plain[2] == 0x81): plain[3..4] = tile number,
    // plain[6..13] = 8 pixels, plain[14..15] == 'V', 'S'. See imageReceiverProcessTile().
    if (plain[2] == 0x81 && plain[14] == 'V' && plain[15] == 'S') {
        uint16_t seq = ((uint16_t)plain[0] << 8) | plain[1];
        uint16_t tile = ((uint16_t)plain[3] << 8) | plain[4];
        // Camera data never moves the replay window: a replayed old chunk would
        // otherwise rewind lastSeq and let old telemetry through again.
        (void)seq;
        DeviceSession* sess = getOrCreateSession(devId);
        if (sess) sess->lastSeen = millis();
        imageReceiverProcessTile(devId, tile, &plain[6]);
        s_lastWasImage = true;
        return false; // camera data, not telemetry
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

    // 7. Replay Attack Protection with 16-bit sequence window
    DeviceSession* sess = getOrCreateSession(devId);
    if (sess) {
        if (sess->hasLastSeq && !isBtn && !isPor) {
            // Handle 16-bit modular sequence wraparound:
            // Reject duplicate or stale packets (diff <= 0)
            int32_t diff = (int32_t)seq - (int32_t)sess->lastSeq;
            if (diff <= 0 && diff > -32768) {
                return false; // Stale or replayed packet!
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
    outTelem->rawP2      = plain[14];
    outTelem->rawRst     = plain[15];
    outTelem->rssi       = rssi;
    outTelem->lqi        = lqi;
    outTelem->valid      = true;

    return true;
}
