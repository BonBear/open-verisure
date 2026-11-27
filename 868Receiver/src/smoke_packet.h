/**
 * @file smoke_packet.h
 * @brief Smoke detector packet types (plain[12] / rawP0) and the cache of its last values.
 *
 * Header-only so the host tests can use it. Types, same as SMOKE_DETECTOR/firmware/src/packet.h:
 *   0x5D status:   rawP1 = status bits (0x01 chamber fault, 0x02 chamber dirty, 0x04 test button,
 *                  0x08 heartbeat, 0x10 hushed); the smoke alarm itself is flags bit 0 (isOpen)
 *   0x5E climate:  rawP1 = temperature, raw * 0.5 - 40 deg C, 0xFF = none (reserved)
 *   0x5F humidity: rawP1 = relative humidity 0..100 %, 0xFF = none (reserved)
 * Call smokeApply() only for IDs that are not a camera or siren (those are told by ID prefix).
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "crypto.h"
#include "sensor_registry.h"

#define SMOKE_TYPE_STATUS   0x5D
#define SMOKE_TYPE_CLIMATE  0x5E
#define SMOKE_TYPE_HUMIDITY 0x5F
#define SMOKE_STATUS_FAULT     0x01   /* chamber fault */
#define SMOKE_STATUS_DIRTY     0x02   /* chamber dirty / maintenance */
#define SMOKE_STATUS_TEST      0x04   /* test button: event only, never cached */
#define SMOKE_STATUS_HEARTBEAT 0x08   /* periodic timer packet */
#define SMOKE_STATUS_HUSHED    0x10   /* alarm hushed */

static inline bool smokeIsPacket(uint8_t rawP0) {
    return rawP0 >= SMOKE_TYPE_STATUS && rawP0 <= SMOKE_TYPE_HUMIDITY;
}

/** Marks telem as smoke and fills what this packet type does not carry from the last record. */
static inline void smokeApply(VerisureTelemetry& telem, const SensorRecord* prev) {
    telem.isSmoke = true;
    if (telem.rawP0 == SMOKE_TYPE_STATUS) {
        telem.smokeStatus = telem.rawP1;
        if (prev) {
            telem.hasTemp = prev->hasTemp;
            telem.tempC = prev->tempC;
            telem.hasHumidity = prev->lastTelem.hasHumidity;
            telem.humidityPct = prev->lastTelem.humidityPct;
        }
        return;
    }
    // climate / humidity: keep the last status, but not the test button event
    if (prev) {
        telem.smokeStatus = prev->lastTelem.smokeStatus & ~SMOKE_STATUS_TEST;
        telem.hasTemp = prev->hasTemp;
        telem.tempC = prev->tempC;
        telem.hasHumidity = prev->lastTelem.hasHumidity;
        telem.humidityPct = prev->lastTelem.humidityPct;
    }
    if (telem.rawP0 == SMOKE_TYPE_CLIMATE && telem.rawP1 != 0xFF) {
        telem.hasTemp = true;
        telem.tempC = (telem.rawP1 * 0.5f) - 40.0f;
    } else if (telem.rawP0 == SMOKE_TYPE_HUMIDITY && telem.rawP1 != 0xFF) {
        telem.hasHumidity = true;
        telem.humidityPct = telem.rawP1 > 100 ? 100 : telem.rawP1;
    }
}
