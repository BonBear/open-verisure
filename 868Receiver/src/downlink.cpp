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
    bool sent;      // included in at least one poll answer
    unsigned long queuedAt;
};

static bool s_armed = false;
static PendingCommand s_pending[DOWNLINK_MAX_CAMERAS];

static void expirePending() {
    unsigned long now = millis();
    for (int i = 0; i < DOWNLINK_MAX_CAMERAS; i++) {
        if (s_pending[i].cmd && now - s_pending[i].queuedAt > DOWNLINK_CMD_TTL_MS) s_pending[i].cmd = 0;
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
        if (s_pending[i].cmd && strncmp(s_pending[i].deviceId, deviceId, 8) == 0) return &s_pending[i];
    }
    return nullptr;
}

bool downlinkRequestPicture(const char* deviceId, uint8_t cmd) {
    if (!deviceId || cmd < 0xD1 || cmd > 0xD3) return false;
    expirePending();
    PendingCommand* p = findPending(deviceId);
    if (!p) {
        for (int i = 0; i < DOWNLINK_MAX_CAMERAS && !p; i++) {
            if (!s_pending[i].cmd) p = &s_pending[i];
        }
    }
    if (!p) return false;
    strncpy(p->deviceId, deviceId, 8);
    p->deviceId[8] = '\0';
    p->cmd = cmd;
    p->sent = false;
    p->queuedAt = millis();
    return true;
}

uint8_t downlinkCommandForPoll(const char* deviceId, uint8_t cameraState) {
    expirePending();
    PendingCommand* p = findPending(deviceId);
    if (!p) return 0;
    if (p->sent && (cameraState & DOWNLINK_STATE_ACK)) {
        p->cmd = 0;             // delivered with an earlier answer
        return 0;
    }
    p->sent = true;
    return p->cmd;
}

void downlinkBuildPlain(uint16_t pollSeq, uint16_t nonce, bool armed, uint8_t cmd,
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
    out[14] = 'V';
    out[15] = 'S';
}
