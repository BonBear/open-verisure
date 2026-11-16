/**
 * @file downlink.cpp
 * @brief Gateway -> camera downlink state (see downlink.h).
 */
#include "downlink.h"

#include <Preferences.h>
#include <string.h>

#define DOWNLINK_MAX_CAMERAS 4
#define DOWNLINK_CMD_TTL_MS  (15UL * 60UL * 1000UL)   // a request not delivered by then is dropped

struct PendingCommand {
    char deviceId[9];
    uint8_t cmd;
    uint8_t resendCount;    // resend request (0 = none): count chunks from resendIdx
    uint16_t resendIdx;
    bool sent;      // included in at least one poll answer
    unsigned long queuedAt;
};

static bool s_armed = false;
static PendingCommand s_pending[DOWNLINK_MAX_CAMERAS];

static void expirePending() {
    unsigned long now = millis();
    for (int i = 0; i < DOWNLINK_MAX_CAMERAS; i++) {
        if ((s_pending[i].cmd || s_pending[i].resendCount) && now - s_pending[i].queuedAt > DOWNLINK_CMD_TTL_MS) {
            s_pending[i].cmd = 0;
            s_pending[i].resendCount = 0;
        }
    }
}


void downlinkInit() {
    Preferences prefs;
    prefs.begin("downlink", true);
    s_armed = prefs.getBool("armed", false);
    prefs.end();
    memset(s_pending, 0, sizeof(s_pending));
}

bool downlinkArmed() {
    return s_armed;
}

void downlinkSetArmed(bool armed) {
    if (armed == s_armed) return;
    s_armed = armed;
    Preferences prefs;
    prefs.begin("downlink", false);
    prefs.putBool("armed", armed);
    prefs.end();
}

static PendingCommand* findPending(const char* deviceId) {
    for (int i = 0; i < DOWNLINK_MAX_CAMERAS; i++) {
        if ((s_pending[i].cmd || s_pending[i].resendCount) && strncmp(s_pending[i].deviceId, deviceId, 8) == 0)
            return &s_pending[i];
    }
    return nullptr;
}

bool downlinkRequestPicture(const char* deviceId, uint8_t cmd) {
    if (!deviceId || ((cmd < 0xD1 || cmd > 0xD6) && cmd != 0xE8)) return false;   // 0xE8: HOLD (service)
    expirePending();
    PendingCommand* p = findPending(deviceId);
    if (!p) {
        for (int i = 0; i < DOWNLINK_MAX_CAMERAS && !p; i++) {
            if (!s_pending[i].cmd && !s_pending[i].resendCount) p = &s_pending[i];
        }
        if (p) p->resendCount = 0;
    }
    if (!p) return false;
    strncpy(p->deviceId, deviceId, 8);
    p->deviceId[8] = '\0';
    p->cmd = cmd;
    p->sent = false;
    p->queuedAt = millis();
    return true;
}

bool downlinkRequestResend(const char* deviceId, uint16_t idx, uint8_t count) {
    if (!deviceId || count == 0 || count > 16) return false;
    expirePending();
    PendingCommand* p = findPending(deviceId);
    if (!p) {
        for (int i = 0; i < DOWNLINK_MAX_CAMERAS && !p; i++) {
            if (!s_pending[i].cmd && !s_pending[i].resendCount) p = &s_pending[i];
        }
        if (!p) return false;
        p->cmd = 0;
        p->sent = false;
        strncpy(p->deviceId, deviceId, 8);
        p->deviceId[8] = '\0';
    }
    p->resendIdx = idx;
    p->resendCount = count;
    p->queuedAt = millis();
    return true;
}

uint8_t downlinkCommandForPoll(const char* deviceId, uint8_t cameraState, uint16_t* arg) {
    if (arg) *arg = 0;
    expirePending();
    PendingCommand* p = findPending(deviceId);
    if (!p) return 0;
    if (p->cmd && p->sent && (cameraState & DOWNLINK_STATE_ACK))
        p->cmd = 0;             // delivered with an earlier answer
    if (p->cmd) {
        p->sent = true;
        return p->cmd;
    }
    if (p->resendCount) {
        uint8_t c = 0xF0 | (uint8_t)(p->resendCount - 1);
        if (arg) *arg = p->resendIdx;
        p->resendCount = 0;     // once: the gateway asks again for what is still missing
        return c;
    }
    return 0;
}

void downlinkBuildPlain(uint16_t pollSeq, uint16_t nonce, bool armed, uint8_t cmd, uint16_t arg,
                        const char* deviceId, uint8_t out[16]) {
    memset(out, 0, 16);
    out[0] = 'D';
    out[1] = 'L';
    out[2] = (uint8_t)(pollSeq >> 8);
    out[3] = (uint8_t)(pollSeq & 0xFF);
    out[4] = (uint8_t)(nonce >> 8);
    out[5] = (uint8_t)(nonce & 0xFF);
    out[6] = armed ? 1 : 0;
    out[7] = cmd;
    memcpy(&out[8], deviceId, 4);
    out[12] = (uint8_t)(arg >> 8);
    out[13] = (uint8_t)(arg & 0xFF);
    out[14] = 'V';
    out[15] = 'S';
}
