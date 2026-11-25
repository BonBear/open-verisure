/**
 * @file downlink.cpp
 * @brief Gateway -> camera downlink state (see downlink.h).
 */
#include "downlink.h"

#include <Preferences.h>
#include <string.h>

#define DOWNLINK_CMD_TTL_MS  (15UL * 60UL * 1000UL)   // a request not delivered by then is dropped

struct PendingCommand {
    char deviceId[9];
    uint8_t cmd;
    uint32_t resendMap;     // resend request (0 = none): bit i = chunk resendFirst + i
    uint16_t resendFirst;
    bool sent;      // included in at least one poll answer
    unsigned long queuedAt;
};

static bool s_armed = false;
static PendingCommand s_pending[DOWNLINK_MAX_DEVICES];

static void expirePending() {
    unsigned long now = millis();
    for (int i = 0; i < DOWNLINK_MAX_DEVICES; i++) {
        if ((s_pending[i].cmd || s_pending[i].resendMap) && now - s_pending[i].queuedAt > DOWNLINK_CMD_TTL_MS) {
            s_pending[i].cmd = 0;
            s_pending[i].resendMap = 0;
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
    for (int i = 0; i < DOWNLINK_MAX_DEVICES; i++) {
        if ((s_pending[i].cmd || s_pending[i].resendMap) && strncmp(s_pending[i].deviceId, deviceId, 8) == 0)
            return &s_pending[i];
    }
    return nullptr;
}

bool downlinkRequestPicture(const char* deviceId, uint8_t cmd) {
    if (!deviceId || ((cmd < 0xD1 || cmd > 0xD6) && cmd != 0xE8)) return false;   // 0xE8: HOLD (service)
    expirePending();
    PendingCommand* p = findPending(deviceId);
    if (!p) {
        for (int i = 0; i < DOWNLINK_MAX_DEVICES && !p; i++) {
            if (!s_pending[i].cmd && !s_pending[i].resendMap) p = &s_pending[i];
        }
        if (p) p->resendMap = 0;
    }
    if (!p) return false;
    strncpy(p->deviceId, deviceId, 8);
    p->deviceId[8] = '\0';
    p->cmd = cmd;
    p->sent = false;
    p->queuedAt = millis();
    return true;
}

bool downlinkRequestSiren(const char* deviceId, bool on, uint8_t durationSec, uint8_t tone) {
    if (!deviceId || deviceId[0] == '\0') return false;
    expirePending();
    PendingCommand* p = findPending(deviceId);
    if (!p) {
        for (int i = 0; i < DOWNLINK_MAX_DEVICES && !p; i++) {
            if (!s_pending[i].cmd && !s_pending[i].resendMap) p = &s_pending[i];
        }
        if (p) p->resendMap = 0;
    }
    if (!p) return false;
    strncpy(p->deviceId, deviceId, 8);
    p->deviceId[8] = '\0';
    p->cmd = on ? DOWNLINK_CMD_SIREN_ON : DOWNLINK_CMD_SIREN_OFF;
    if (on) {
        uint8_t t = (tone <= DOWNLINK_SIREN_MAX_TONE) ? tone : 0;
        uint16_t dur = durationSec;
        if (t == 0 && dur == 0) dur = 60;
        if (dur > DOWNLINK_SIREN_MAX_SEC) dur = DOWNLINK_SIREN_MAX_SEC;
        p->resendFirst = (uint16_t)(((uint16_t)t << 8) | (dur & 0xFFU));
    } else {
        p->resendFirst = 0;
    }
    p->sent = false;
    p->queuedAt = millis();
    return true;
}

bool downlinkRequestResend(const char* deviceId, uint16_t first, uint32_t map) {
    if (!deviceId || map == 0) return false;
    expirePending();
    PendingCommand* p = findPending(deviceId);
    if (!p) {
        for (int i = 0; i < DOWNLINK_MAX_DEVICES && !p; i++) {
            if (!s_pending[i].cmd && !s_pending[i].resendMap) p = &s_pending[i];
        }
        if (!p) return false;
        p->cmd = 0;
        p->sent = false;
        strncpy(p->deviceId, deviceId, 8);
        p->deviceId[8] = '\0';
    }
    p->resendFirst = first;
    p->resendMap = map;
    p->queuedAt = millis();
    return true;
}

void downlinkCancelResend(const char* deviceId) {
    if (!deviceId) return;
    PendingCommand* p = findPending(deviceId);
    if (p) p->resendMap = 0;        // the slot is free again when no command is queued
}

bool downlinkResendPending(const char* deviceId) {
    if (!deviceId) return false;
    expirePending();
    PendingCommand* p = findPending(deviceId);
    return p && p->resendMap;
}

uint8_t downlinkCommandForPoll(const char* deviceId, uint8_t cameraState, uint16_t* first, uint32_t* map) {
    if (first) *first = 0;
    if (map) *map = 0;
    expirePending();
    PendingCommand* p = findPending(deviceId);
    if (!p) return 0;
    // The camera's ACK bit only says "the last answer I received carried a command", not which
    // one. So a command goes out the first time only in the answer to a poll WITHOUT the ACK bit
    // (the camera's last received answer had none); every ACK after that can only be for this
    // command. Otherwise an ACK left over from an older command, whose answer to it was lost,
    // would clear a newer command that never arrived. While the camera still reports an old ACK
    // it gets an answer without a command (it clears the bit when it receives one): at most one
    // more poll.
    if (p->cmd && p->sent && (cameraState & DOWNLINK_STATE_ACK))
        p->cmd = 0;             // delivered with an earlier answer
    // A pending resend request goes before ANY command, also one already sent. The camera takes
    // an answer without a resend request as the acknowledgement of the open picture, so a command
    // first would end that picture with holes (and break the delta base). This is ACK-safe: the
    // ACK check above ran first, the camera sets the ACK bit only for 0xD1..0xD6/0xE8 and a
    // 0xF0 answer clears it, and a sent command without ACK in the poll was not received. A
    // command cannot starve: the rounds are bounded by RS_MAX_ROUNDS / RS_TOTAL_MS in
    // image_receiver.cpp, which also cancels a request it gives up (downlinkCancelResend).
    if (p->resendMap) {
        if (first) *first = p->resendFirst;
        if (map) *map = p->resendMap;
        p->resendMap = 0;       // once: the gateway asks again for what is still missing
        return DOWNLINK_CMD_RESEND;
    }
    if (p->cmd && !(cameraState & DOWNLINK_STATE_ACK)) {
        p->sent = true;
        if (p->cmd == DOWNLINK_CMD_SIREN_ON && first) *first = p->resendFirst;
        return p->cmd;
    }
    return 0;
}

void downlinkBuildPlain(uint32_t pollCounter, bool armed, uint8_t cmd, uint16_t first, uint32_t map,
                        uint8_t out[16], uint8_t flags, uint8_t channel) {
    memset(out, 0, 16);
    out[0] = 'D';
    out[1] = 'L';
    out[2] = (uint8_t)(pollCounter >> 24);
    out[3] = (uint8_t)(pollCounter >> 16);
    out[4] = (uint8_t)(pollCounter >> 8);
    out[5] = (uint8_t)pollCounter;
    out[6] = armed ? 1 : 0;
    out[7] = cmd;
    if (cmd == DOWNLINK_CMD_RESEND) {
        out[8] = (uint8_t)(first >> 8);
        out[9] = (uint8_t)first;
        for (int i = 0; i < 4; i++) out[10 + i] = (uint8_t)(map >> (8 * i));
    } else if (cmd == DOWNLINK_CMD_SIREN_ON) {
        out[8] = (first > DOWNLINK_SIREN_MAX_SEC) ? (uint8_t)DOWNLINK_SIREN_MAX_SEC : (uint8_t)first;
        out[9] = (channel <= DOWNLINK_SIREN_MAX_TONE) ? channel : 0;
    } else {
        out[8] = flags;
        out[9] = (flags & DL_FLAG_PCH) ? channel : 0;
    }
    out[14] = 'V';
    out[15] = 'S';
}
