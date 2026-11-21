/**
 * @file picture_token.cpp
 * @brief Phase D single-flight RF Picture Token state machine implementation.
 */
#include "picture_token.h"

#include <string.h>

namespace {

struct WaitEntry {
    char id[9];
    uint32_t lastReqMs;
};

struct PicTokenCtx {
    PicTokenState state;
    bool holderLegacy;
    bool activitySeen;
    char holderId[9];
    uint32_t grantedAtMs;
    uint32_t leaseStartMs;
    uint32_t leaseDurMs;
    uint8_t waitCount;
    WaitEntry waitQueue[PIC_TOKEN_MAX_CAMERAS];
    uint32_t grants;
    uint32_t holds;
    uint32_t releases;
    uint32_t expiries;
    uint32_t legacyClaims;
};

PicTokenCtx s_tok;

inline bool validId(const char* deviceId) {
    return deviceId && deviceId[0] != '\0';
}

inline void copyId(char dst[9], const char* src) {
    memset(dst, 0, 9);
    if (src) strncpy(dst, src, 8);
}

inline bool elapsed(uint32_t nowMs, uint32_t sinceMs, uint32_t durMs) {
    return (uint32_t)(nowMs - sinceMs) >= durMs;
}

int findWaiter(const char id[9]) {
    for (uint8_t i = 0; i < s_tok.waitCount; i++) {
        if (strcmp(s_tok.waitQueue[i].id, id) == 0) return i;
    }
    return -1;
}

void removeWaiterAt(uint8_t idx) {
    for (uint8_t i = idx; i + 1 < s_tok.waitCount; i++) {
        s_tok.waitQueue[i] = s_tok.waitQueue[i + 1];
    }
    if (s_tok.waitCount > 0) {
        s_tok.waitCount--;
        memset(&s_tok.waitQueue[s_tok.waitCount], 0, sizeof(WaitEntry));
    }
}

void removeWaiter(const char id[9]) {
    int idx = findWaiter(id);
    if (idx >= 0) removeWaiterAt((uint8_t)idx);
}

void pruneWaiters(uint32_t nowMs) {
    uint8_t dst = 0;
    for (uint8_t i = 0; i < s_tok.waitCount; i++) {
        if (!elapsed(nowMs, s_tok.waitQueue[i].lastReqMs, PIC_TOKEN_WAIT_TTL_MS)) {
            if (dst != i) s_tok.waitQueue[dst] = s_tok.waitQueue[i];
            dst++;
        }
    }
    for (uint8_t i = dst; i < s_tok.waitCount; i++) {
        memset(&s_tok.waitQueue[i], 0, sizeof(WaitEntry));
    }
    s_tok.waitCount = dst;
}

void promoteWaiter(uint32_t nowMs) {
    if (s_tok.waitCount > 0) {
        copyId(s_tok.holderId, s_tok.waitQueue[0].id);
        removeWaiterAt(0);
        s_tok.state = PIC_TOK_RESERVED;
        s_tok.holderLegacy = false;
        s_tok.activitySeen = false;
        s_tok.grantedAtMs = 0;
        s_tok.leaseStartMs = nowMs;
        s_tok.leaseDurMs = PIC_TOKEN_RESERVE_MS;
    } else {
        s_tok.state = PIC_TOK_IDLE;
        memset(s_tok.holderId, 0, sizeof(s_tok.holderId));
        s_tok.holderLegacy = false;
        s_tok.activitySeen = false;
        s_tok.grantedAtMs = 0;
        s_tok.leaseStartMs = 0;
        s_tok.leaseDurMs = 0;
    }
}

void grantHeld(const char id[9], uint32_t nowMs) {
    s_tok.state = PIC_TOK_HELD;
    copyId(s_tok.holderId, id);
    s_tok.holderLegacy = false;
    s_tok.activitySeen = false;
    s_tok.grantedAtMs = nowMs;
    s_tok.leaseStartMs = nowMs;
    s_tok.leaseDurMs = PIC_TOKEN_GRANT_LEASE_MS;
}

void claimActivity(const char* deviceId, uint32_t nowMs) {
    if (!validId(deviceId)) return;
    pictureTokenTick(nowMs);
    char id[9];
    copyId(id, deviceId);
    removeWaiter(id);

    if (s_tok.state == PIC_TOK_IDLE) {
        s_tok.state = PIC_TOK_HELD;
        copyId(s_tok.holderId, id);
        s_tok.holderLegacy = true;
        s_tok.activitySeen = true;
        s_tok.grantedAtMs = nowMs;
        s_tok.leaseStartMs = nowMs;
        s_tok.leaseDurMs = PIC_TOKEN_LEASE_MS;
        s_tok.legacyClaims++;
    } else if (s_tok.state == PIC_TOK_RESERVED) {
        if (strcmp(s_tok.holderId, id) == 0) {
            s_tok.state = PIC_TOK_HELD;
            s_tok.holderLegacy = false;
            s_tok.activitySeen = true;
            s_tok.grantedAtMs = nowMs;
            s_tok.leaseStartMs = nowMs;
            s_tok.leaseDurMs = PIC_TOKEN_LEASE_MS;
        } else {
            // Legacy camera starts without 0xBB while another camera holds RESERVED:
            // push displaced camera back to the front of the wait queue.
            char displaced[9];
            copyId(displaced, s_tok.holderId);
            uint8_t keep = (s_tok.waitCount < PIC_TOKEN_MAX_CAMERAS)
                         ? s_tok.waitCount
                         : (PIC_TOKEN_MAX_CAMERAS - 1);
            for (uint8_t i = keep; i > 0; i--) {
                s_tok.waitQueue[i] = s_tok.waitQueue[i - 1];
            }
            copyId(s_tok.waitQueue[0].id, displaced);
            s_tok.waitQueue[0].lastReqMs = nowMs;
            s_tok.waitCount = keep + 1;

            s_tok.state = PIC_TOK_HELD;
            copyId(s_tok.holderId, id);
            s_tok.holderLegacy = true;
            s_tok.activitySeen = true;
            s_tok.grantedAtMs = nowMs;
            s_tok.leaseStartMs = nowMs;
            s_tok.leaseDurMs = PIC_TOKEN_LEASE_MS;
            s_tok.legacyClaims++;
        }
    } else if (strcmp(s_tok.holderId, id) == 0) {
        s_tok.activitySeen = true;
        s_tok.leaseStartMs = nowMs;
        s_tok.leaseDurMs = PIC_TOKEN_LEASE_MS;
    }
}

}  // namespace

void pictureTokenInit() {
    memset(&s_tok, 0, sizeof(s_tok));
    s_tok.state = PIC_TOK_IDLE;
}

bool pictureTokenTick(uint32_t nowMs, char* expiredId, size_t expiredIdSize) {
    pruneWaiters(nowMs);
    bool expired = false;
    if (s_tok.state == PIC_TOK_RESERVED) {
        expired = elapsed(nowMs, s_tok.leaseStartMs, s_tok.leaseDurMs);
    } else if (s_tok.state == PIC_TOK_HELD) {
        expired = elapsed(nowMs, s_tok.leaseStartMs, s_tok.leaseDurMs) ||
                  elapsed(nowMs, s_tok.grantedAtMs, PIC_TOKEN_MAX_HOLD_MS);
    }
    if (!expired) return false;
    if (expiredId && expiredIdSize > 0) {
        strncpy(expiredId, s_tok.holderId, expiredIdSize - 1);
        expiredId[expiredIdSize - 1] = '\0';
    }
    s_tok.expiries++;
    promoteWaiter(nowMs);
    return true;
}

bool pictureTokenRequest(const char* deviceId, uint32_t nowMs) {
    if (!validId(deviceId)) return false;
    pictureTokenTick(nowMs);
    char id[9];
    copyId(id, deviceId);

    if (s_tok.state == PIC_TOK_IDLE) {
        removeWaiter(id);
        grantHeld(id, nowMs);
        s_tok.grants++;
        return true;
    }
    if (s_tok.state == PIC_TOK_RESERVED && strcmp(s_tok.holderId, id) == 0) {
        removeWaiter(id);
        grantHeld(id, nowMs);
        s_tok.grants++;
        return true;
    }
    if (s_tok.state == PIC_TOK_HELD && strcmp(s_tok.holderId, id) == 0) {
        if (!s_tok.activitySeen) {
            // Lost GRANT retry: refresh lease while keeping grantedAtMs unchanged.
            s_tok.leaseStartMs = nowMs;
            s_tok.leaseDurMs = PIC_TOKEN_GRANT_LEASE_MS;
        } else {
            // Previous 0xB1 was lost and the same camera started a new picture.
            grantHeld(id, nowMs);
        }
        s_tok.grants++;
        return true;
    }

    int idx = findWaiter(id);
    if (idx >= 0) {
        s_tok.waitQueue[idx].lastReqMs = nowMs;
    } else if (s_tok.waitCount < PIC_TOKEN_MAX_CAMERAS) {
        copyId(s_tok.waitQueue[s_tok.waitCount].id, id);
        s_tok.waitQueue[s_tok.waitCount].lastReqMs = nowMs;
        s_tok.waitCount++;
    }
    s_tok.holds++;
    return false;
}

bool pictureTokenRelease(const char* deviceId, uint32_t nowMs) {
    if (!validId(deviceId)) return false;
    pictureTokenTick(nowMs);
    char id[9];
    copyId(id, deviceId);
    removeWaiter(id);

    if ((s_tok.state == PIC_TOK_HELD || s_tok.state == PIC_TOK_RESERVED) &&
        strcmp(s_tok.holderId, id) == 0) {
        if (s_tok.state == PIC_TOK_HELD) s_tok.releases++;
        promoteWaiter(nowMs);
        return true;
    }
    return false;
}

void pictureTokenOnPicStart(const char* deviceId, uint32_t nowMs) {
    claimActivity(deviceId, nowMs);
}

void pictureTokenOnChunk(const char* deviceId, bool isResend, uint32_t nowMs) {
    (void)isResend;
    claimActivity(deviceId, nowMs);
}

void pictureTokenOnPictureDone(const char* deviceId, bool resendActive, uint32_t nowMs) {
    if (!validId(deviceId)) return;
    pictureTokenTick(nowMs);
    char id[9];
    copyId(id, deviceId);
    if (s_tok.state == PIC_TOK_HELD && strcmp(s_tok.holderId, id) == 0) {
        if (resendActive) {
            s_tok.leaseStartMs = nowMs;
            s_tok.leaseDurMs = PIC_TOKEN_LEASE_MS;
        } else if (s_tok.holderLegacy) {
            s_tok.releases++;
            promoteWaiter(nowMs);
        }
    }
}

void pictureTokenOnPollAnswered(const char* deviceId, bool hasResend, uint32_t nowMs) {
    if (!validId(deviceId)) return;
    pictureTokenTick(nowMs);
    char id[9];
    copyId(id, deviceId);
    if (s_tok.state == PIC_TOK_HELD && strcmp(s_tok.holderId, id) == 0) {
        if (hasResend) {
            s_tok.leaseStartMs = nowMs;
            s_tok.leaseDurMs = PIC_TOKEN_LEASE_MS;
        } else if (s_tok.activitySeen) {
            if (s_tok.holderLegacy) {
                s_tok.releases++;
                promoteWaiter(nowMs);
            } else {
                uint32_t el = (uint32_t)(nowMs - s_tok.leaseStartMs);
                uint32_t rem = (el < s_tok.leaseDurMs) ? (s_tok.leaseDurMs - el) : 0;
                if (rem > PIC_TOKEN_ACK_GRACE_MS) {
                    s_tok.leaseStartMs = nowMs;
                    s_tok.leaseDurMs = PIC_TOKEN_ACK_GRACE_MS;
                }
            }
        }
    }
}

void pictureTokenGetStatus(uint32_t nowMs, PicTokenStatus* out) {
    pictureTokenTick(nowMs);
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->state = s_tok.state;
    copyId(out->holderId, s_tok.holderId);
    out->holderLegacy = s_tok.holderLegacy;
    if (s_tok.state == PIC_TOK_HELD) {
        uint32_t holdEl = (uint32_t)(nowMs - s_tok.grantedAtMs);
        uint32_t leaseEl = (uint32_t)(nowMs - s_tok.leaseStartMs);
        uint32_t remLease = (leaseEl < s_tok.leaseDurMs) ? (s_tok.leaseDurMs - leaseEl) : 0;
        uint32_t remMax = (holdEl < PIC_TOKEN_MAX_HOLD_MS) ? (PIC_TOKEN_MAX_HOLD_MS - holdEl) : 0;
        out->holdMs = holdEl;
        out->leaseRemMs = (remLease < remMax) ? remLease : remMax;
    } else if (s_tok.state == PIC_TOK_RESERVED) {
        uint32_t leaseEl = (uint32_t)(nowMs - s_tok.leaseStartMs);
        out->holdMs = 0;
        out->leaseRemMs = (leaseEl < s_tok.leaseDurMs) ? (s_tok.leaseDurMs - leaseEl) : 0;
    }
    out->waitCount = s_tok.waitCount;
    for (uint8_t i = 0; i < s_tok.waitCount; i++) {
        copyId(out->waitIds[i], s_tok.waitQueue[i].id);
    }
    out->grants = s_tok.grants;
    out->holds = s_tok.holds;
    out->releases = s_tok.releases;
    out->expiries = s_tok.expiries;
    out->legacyClaims = s_tok.legacyClaims;
}
