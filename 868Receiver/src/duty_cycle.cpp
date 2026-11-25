/**
 * @file duty_cycle.cpp
 * @brief Phase E sliding-window RF duty-cycle accounting & band-50 fallback tracking.
 */
#include "duty_cycle.h"

#include <stdlib.h>
#include <string.h>

namespace {

struct BandTrack {
    uint32_t bucketsUs[DUTY_BUCKETS];
    uint64_t sumUs;
    uint32_t peak1hMs;
    uint32_t overBudgetCount;
    bool     overBudget;
};

struct CamSlot {
    bool      used;
    bool      picCountedFallback;
    char      deviceId[9];
    uint32_t  lastUseMs;
    BandTrack b50;
    BandTrack b54;
    uint32_t  pkt24Count;
    uint32_t  pchChunks;
    uint32_t  fallbackChunks;
    uint32_t  fallbackPictures;
    uint64_t  fallbackTotalUs;
};

struct DutyState {
    bool      initialized;
    uint32_t  bucketStartMs;
    uint8_t   curIdx;
    BandTrack gw;
    uint32_t  gwTxPackets;
    uint64_t  gwTxTotalUs;
    BandTrack gwB54;
    uint32_t  gwB54Packets;
    uint64_t  gwB54TotalUs;
    uint32_t  totalFallbackPictures;
    uint32_t  totalFallbackChunks;
    uint64_t  totalFallbackUs;
    uint32_t  totalCamB50OverBudgetCount;
    CamSlot   cameras[DUTY_MAX_CAMERAS];
};

// Heap-allocated once to keep .dram0.bss growth to a single pointer (4 bytes on ESP32-S2).
DutyState* s_st = nullptr;

DutyState* getState() {
    if (!s_st) {
        s_st = static_cast<DutyState*>(calloc(1, sizeof(DutyState)));
    }
    return s_st;
}

inline uint32_t usToMs(uint64_t us) {
    return (uint32_t)((us + 500ULL) / 1000ULL);
}

inline uint32_t pctTenths(uint64_t sumUs, uint64_t limitUs) {
    return (uint32_t)((sumUs * 1000ULL) / limitUs);
}

inline bool validId(const char* deviceId) {
    return deviceId && deviceId[0] != '\0';
}

void copyId(char dst[9], const char* src) {
    size_t i = 0;
    if (src) {
        for (; i < 8 && src[i] != '\0'; i++) {
            dst[i] = src[i];
        }
    }
    for (; i < 9; i++) {
        dst[i] = '\0';
    }
}

void recomputeBand(BandTrack& b, uint64_t limitUs) {
    uint64_t sum = 0;
    for (uint8_t i = 0; i < DUTY_BUCKETS; i++) {
        sum += b.bucketsUs[i];
    }
    b.sumUs = sum;
    if (b.sumUs <= limitUs) {
        b.overBudget = false;
    }
}

void addAirtime(BandTrack& b, uint8_t curIdx, uint32_t us, uint64_t limitUs,
                uint32_t* monotonicOverCount) {
    const uint32_t cur = b.bucketsUs[curIdx];
    const uint32_t added = (UINT32_MAX - cur < us) ? (UINT32_MAX - cur) : us;
    b.bucketsUs[curIdx] = cur + added;
    b.sumUs += added;

    const uint32_t ms = usToMs(b.sumUs);
    if (ms > b.peak1hMs) {
        b.peak1hMs = ms;
    }
    if (b.sumUs > limitUs && !b.overBudget) {
        b.overBudget = true;
        b.overBudgetCount++;
        if (monotonicOverCount) {
            (*monotonicOverCount)++;
        }
    }
}

void advanceClock(DutyState* st, uint32_t nowMs) {
    if (!st->initialized) {
        st->initialized = true;
        st->bucketStartMs = nowMs;
        st->curIdx = 0;
        return;
    }

    const uint32_t elapsed = (uint32_t)(nowMs - st->bucketStartMs);
    if (elapsed < DUTY_BUCKET_MS) {
        return;
    }

    const uint32_t steps = elapsed / DUTY_BUCKET_MS;
    if (steps >= DUTY_BUCKETS) {
        memset(st->gw.bucketsUs, 0, sizeof(st->gw.bucketsUs));
        memset(st->gwB54.bucketsUs, 0, sizeof(st->gwB54.bucketsUs));
        for (uint8_t i = 0; i < DUTY_MAX_CAMERAS; i++) {
            if (!st->cameras[i].used) continue;
            memset(st->cameras[i].b50.bucketsUs, 0, sizeof(st->cameras[i].b50.bucketsUs));
            memset(st->cameras[i].b54.bucketsUs, 0, sizeof(st->cameras[i].b54.bucketsUs));
        }
        st->curIdx = 0;
    } else {
        for (uint32_t s = 0; s < steps; s++) {
            st->curIdx = (uint8_t)((st->curIdx + 1U) % DUTY_BUCKETS);
            st->gw.bucketsUs[st->curIdx] = 0;
            st->gwB54.bucketsUs[st->curIdx] = 0;
            for (uint8_t i = 0; i < DUTY_MAX_CAMERAS; i++) {
                if (!st->cameras[i].used) continue;
                st->cameras[i].b50.bucketsUs[st->curIdx] = 0;
                st->cameras[i].b54.bucketsUs[st->curIdx] = 0;
            }
        }
    }

    st->bucketStartMs = (uint32_t)(nowMs - (elapsed % DUTY_BUCKET_MS));

    recomputeBand(st->gw, DUTY_BAND50_LIMIT_US);
    recomputeBand(st->gwB54, DUTY_BAND54_LIMIT_US);
    for (uint8_t i = 0; i < DUTY_MAX_CAMERAS; i++) {
        if (!st->cameras[i].used) continue;
        recomputeBand(st->cameras[i].b50, DUTY_BAND50_LIMIT_US);
        recomputeBand(st->cameras[i].b54, DUTY_BAND54_LIMIT_US);
    }
}

CamSlot* acquireCamSlot(DutyState* st, const char* deviceId, uint32_t nowMs) {
    char id[9];
    copyId(id, deviceId);

    CamSlot* freeSlot = nullptr;
    CamSlot* lru = nullptr;
    uint32_t oldestAge = 0;

    for (uint8_t i = 0; i < DUTY_MAX_CAMERAS; i++) {
        CamSlot* s = &st->cameras[i];
        if (!s->used) {
            if (!freeSlot) freeSlot = s;
            continue;
        }
        if (strcmp(s->deviceId, id) == 0) {
            return s;
        }
        const uint32_t age = (uint32_t)(nowMs - s->lastUseMs);
        if (!lru || age > oldestAge) {
            lru = s;
            oldestAge = age;
        }
    }

    CamSlot* target = freeSlot ? freeSlot : lru;
    memset(target, 0, sizeof(*target));
    target->used = true;
    copyId(target->deviceId, id);
    target->lastUseMs = nowMs;
    return target;
}

}  // namespace

void dutyCycleInit() {
    DutyState* st = getState();
    if (st) {
        memset(st, 0, sizeof(*st));
    }
}

void dutyCycleTick(uint32_t nowMs) {
    DutyState* st = getState();
    if (!st) return;
    advanceClock(st, nowMs);
}

void dutyCycleRecordGwTx(uint32_t nowMs) {
    DutyState* st = getState();
    if (!st) return;
    advanceClock(st, nowMs);
    st->gwTxPackets++;
    st->gwTxTotalUs += DUTY_AIRTIME_PKT24_US;
    addAirtime(st->gw, st->curIdx, DUTY_AIRTIME_PKT24_US, DUTY_BAND50_LIMIT_US, nullptr);
}

void dutyCycleRecordGwWakeTx(uint32_t nowMs) {
    DutyState* st = getState();
    if (!st) return;
    advanceClock(st, nowMs);
    st->gwB54Packets++;
    st->gwB54TotalUs += DUTY_AIRTIME_WAKE6_US;
    addAirtime(st->gwB54, st->curIdx, DUTY_AIRTIME_WAKE6_US, DUTY_BAND54_LIMIT_US, nullptr);
}

bool dutyCycleAllowGwWake(uint32_t nowMs) {
    DutyState* st = getState();
    if (!st) return false;
    advanceClock(st, nowMs);
    return (st->gwB54.sumUs + 1600000ULL) < (uint64_t)DUTY_BAND54_LIMIT_US;
}

void dutyCycleRecordCamPkt24(const char* deviceId, uint32_t nowMs) {
    if (!validId(deviceId)) return;
    DutyState* st = getState();
    if (!st) return;
    advanceClock(st, nowMs);
    CamSlot* cam = acquireCamSlot(st, deviceId, nowMs);
    cam->lastUseMs = nowMs;
    cam->pkt24Count++;
    addAirtime(cam->b50, st->curIdx, DUTY_AIRTIME_PKT24_US, DUTY_BAND50_LIMIT_US,
               &st->totalCamB50OverBudgetCount);
}

void dutyCycleRecordCamChunk(const char* deviceId, uint8_t channel, bool isResend,
                             bool isNewPicture, uint32_t nowMs) {
    if (!validId(deviceId)) return;
    DutyState* st = getState();
    if (!st) return;
    advanceClock(st, nowMs);
    CamSlot* cam = acquireCamSlot(st, deviceId, nowMs);
    cam->lastUseMs = nowMs;

    if (isNewPicture) {
        cam->picCountedFallback = false;
    }

    if (channel != 1) {
        addAirtime(cam->b50, st->curIdx, DUTY_AIRTIME_CHUNK56_US, DUTY_BAND50_LIMIT_US,
                   &st->totalCamB50OverBudgetCount);
        cam->fallbackTotalUs += DUTY_AIRTIME_CHUNK56_US;
        st->totalFallbackUs += DUTY_AIRTIME_CHUNK56_US;
        cam->fallbackChunks++;
        st->totalFallbackChunks++;
        if (!isResend && !cam->picCountedFallback) {
            cam->picCountedFallback = true;
            cam->fallbackPictures++;
            st->totalFallbackPictures++;
        }
    } else {
        addAirtime(cam->b54, st->curIdx, DUTY_AIRTIME_CHUNK56_US, DUTY_BAND54_LIMIT_US, nullptr);
        cam->pchChunks++;
    }
}

void dutyCycleGetStatus(uint32_t nowMs, DutyCycleStatus* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    DutyState* st = getState();
    if (!st) return;

    if (st->initialized) {
        advanceClock(st, nowMs);
        out->curBucketAgeMs = (uint32_t)(nowMs - st->bucketStartMs);
    }

    out->gwTx1hUs = st->gw.sumUs;
    out->gwTx1hMs = usToMs(st->gw.sumUs);
    out->gwTxBudgetPctTenths = pctTenths(st->gw.sumUs, DUTY_BAND50_LIMIT_US);
    out->gwTxPeak1hMs = st->gw.peak1hMs;
    out->gwTxOverBudgetCount = st->gw.overBudgetCount;
    out->gwTxOverBudget = st->gw.overBudget;
    out->gwTxPackets = st->gwTxPackets;
    out->gwTxTotalMs = usToMs(st->gwTxTotalUs);

    out->gwB54WindowUs = st->gwB54.sumUs;
    out->gwB54_1hMs = usToMs(st->gwB54.sumUs);
    out->gwB54PctTenths = pctTenths(st->gwB54.sumUs, DUTY_BAND54_LIMIT_US);
    out->gwB54Packets = st->gwB54Packets;
    out->gwB54TotalMs = usToMs(st->gwB54TotalUs);
    out->gwB54OverBudget = st->gwB54.overBudget;

    out->totalFallbackPictures = st->totalFallbackPictures;
    out->totalFallbackChunks = st->totalFallbackChunks;
    out->totalFallbackAirtimeMs = usToMs(st->totalFallbackUs);
    out->totalCamB50OverBudgetCount = st->totalCamB50OverBudgetCount;
    out->anyB50OverBudget = st->gw.overBudget;

    uint8_t n = 0;
    for (uint8_t i = 0; i < DUTY_MAX_CAMERAS; i++) {
        const CamSlot& s = st->cameras[i];
        if (!s.used) continue;
        CamDutyStatus& dst = out->cameras[n++];
        copyId(dst.deviceId, s.deviceId);
        dst.b50_1hUs = s.b50.sumUs;
        dst.b50_1hMs = usToMs(s.b50.sumUs);
        dst.b50BudgetPctTenths = pctTenths(s.b50.sumUs, DUTY_BAND50_LIMIT_US);
        dst.b50Peak1hMs = s.b50.peak1hMs;
        dst.b50OverBudgetCount = s.b50.overBudgetCount;
        dst.b50OverBudget = s.b50.overBudget;
        dst.b54_1hUs = s.b54.sumUs;
        dst.b54_1hMs = usToMs(s.b54.sumUs);
        dst.b54BudgetPctTenths = pctTenths(s.b54.sumUs, DUTY_BAND54_LIMIT_US);
        dst.b54Peak1hMs = s.b54.peak1hMs;
        dst.b54OverBudgetCount = s.b54.overBudgetCount;
        dst.b54OverBudget = s.b54.overBudget;
        dst.pkt24Count = s.pkt24Count;
        dst.pchChunks = s.pchChunks;
        dst.fallbackChunks = s.fallbackChunks;
        dst.fallbackPictures = s.fallbackPictures;
        dst.fallbackAirtimeMs = usToMs(s.fallbackTotalUs);
        if (s.b50.overBudget) {
            out->anyB50OverBudget = true;
        }
    }
    out->cameraCount = n;
}
