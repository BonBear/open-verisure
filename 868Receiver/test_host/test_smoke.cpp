/**
 * @file test_smoke.cpp
 * @brief Host tests of smoke_packet.h: which rawP0 values are smoke packets and how smokeApply()
 *        carries status, temperature and humidity from the previous record.
 */

#include "test.h"
#include "smoke_packet.h"
#include <string.h>

namespace {
VerisureTelemetry mk(uint8_t p0, uint8_t p1) {
    VerisureTelemetry t = {};
    strcpy(t.deviceId, "5MKE0001");
    t.rawP0 = p0;
    t.rawP1 = p1;
    t.valid = true;
    return t;
}

/** What sensorRegistryUpdate() keeps: the last telemetry and the last temperature. */
SensorRecord record(const VerisureTelemetry& t) {
    SensorRecord r = {};
    r.active = true;
    r.lastTelem = t;
    r.hasTemp = t.hasTemp;
    r.tempC = t.tempC;
    return r;
}
}  // namespace

TEST(smoke_packet_type_range) {
    CHECK(!smokeIsPacket(0x5C));
    CHECK(smokeIsPacket(0x5D));
    CHECK(smokeIsPacket(0x5E));
    CHECK(smokeIsPacket(0x5F));
    CHECK(!smokeIsPacket(0x60));
}

TEST(smoke_door_p0_values_are_never_smoke) {
    // door firmware drives P0_2/P0_4 low before sampling: bits 0x14 are always 0
    for (int p0 = 0; p0 < 256; p0++) {
        if ((p0 & 0x14) == 0x14) continue;      // not producible by the door firmware
        CHECK(!smokeIsPacket((uint8_t)p0));
    }
    CHECK(!smokeIsPacket(0xB6));                // siren temperature
    CHECK(!smokeIsPacket(0x00));
}

TEST(smoke_status_without_history) {
    VerisureTelemetry t = mk(0x5D, 0x08 | 0x02);
    smokeApply(t, nullptr);
    CHECK(t.isSmoke);
    CHECK_EQ(t.smokeStatus, 0x0A);
    CHECK(!t.hasTemp);
    CHECK(!t.hasHumidity);
}

TEST(smoke_status_then_climate) {
    VerisureTelemetry s = mk(0x5D, 0x08 | 0x01 | 0x10);
    smokeApply(s, nullptr);
    SensorRecord r = record(s);

    VerisureTelemetry c = mk(0x5E, 100);        // 100 * 0.5 - 40 = 10 C
    smokeApply(c, &r);
    CHECK(c.isSmoke);
    CHECK(c.hasTemp);
    CHECK(c.tempC > 9.99f && c.tempC < 10.01f);
    CHECK_EQ(c.smokeStatus, 0x08 | 0x01 | 0x10);
    r = record(c);

    VerisureTelemetry h = mk(0x5F, 55);
    smokeApply(h, &r);
    CHECK(h.hasHumidity);
    CHECK_EQ(h.humidityPct, 55);
    CHECK(h.hasTemp);
    CHECK(h.tempC > 9.99f && h.tempC < 10.01f);
    CHECK_EQ(h.smokeStatus, 0x08 | 0x01 | 0x10);
}

TEST(smoke_climate_then_status_keeps_values) {
    VerisureTelemetry c = mk(0x5E, 90);         // 5 C
    smokeApply(c, nullptr);
    SensorRecord r = record(c);
    VerisureTelemetry h = mk(0x5F, 40);
    smokeApply(h, &r);
    r = record(h);

    VerisureTelemetry s = mk(0x5D, 0x08);
    smokeApply(s, &r);
    CHECK_EQ(s.smokeStatus, 0x08);
    CHECK(s.hasTemp);
    CHECK(s.tempC > 4.99f && s.tempC < 5.01f);
    CHECK(s.hasHumidity);
    CHECK_EQ(s.humidityPct, 40);
}

TEST(smoke_test_bit_is_not_cached) {
    VerisureTelemetry s = mk(0x5D, 0x04 | 0x01);    // test button + chamber fault
    smokeApply(s, nullptr);
    CHECK_EQ(s.smokeStatus, 0x05);
    SensorRecord r = record(s);

    VerisureTelemetry c = mk(0x5E, 80);
    smokeApply(c, &r);
    CHECK_EQ(c.smokeStatus, 0x01);                  // fault stays, test event is gone
}

TEST(smoke_none_values_keep_previous) {
    VerisureTelemetry c = mk(0x5E, 100);
    smokeApply(c, nullptr);
    VerisureTelemetry h = mk(0x5F, 30);
    SensorRecord r = record(c);
    smokeApply(h, &r);
    r = record(h);

    VerisureTelemetry cn = mk(0x5E, 0xFF);
    smokeApply(cn, &r);
    CHECK(cn.hasTemp);
    CHECK(cn.tempC > 9.99f && cn.tempC < 10.01f);
    CHECK(cn.hasHumidity);
    CHECK_EQ(cn.humidityPct, 30);

    VerisureTelemetry hn = mk(0x5F, 0xFF);
    smokeApply(hn, &r);
    CHECK(hn.hasHumidity);
    CHECK_EQ(hn.humidityPct, 30);

    VerisureTelemetry first = mk(0x5F, 0xFF);       // none and no history: not set
    smokeApply(first, nullptr);
    CHECK(first.isSmoke);
    CHECK(!first.hasHumidity);
    CHECK(!first.hasTemp);
}
