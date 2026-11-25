/**
 * @file main.cpp
 * @brief Verisure 868 MHz Sub-1GHz Gateway Firmware (ESP32-S2 & CC1101)
 *
 * Receives, decrypts, and authenticates 868.950 MHz radio packets from Verisure
 * sensors, and republishes telemetry to Home Assistant via MQTT with Auto-Discovery.
 *
 * ============================================================================
 * ARCHITECTURE & SUBSYSTEMS:
 * ============================================================================
 * 1. Hardware Transceiver (CC1101 SPI):
 *    - Frequency: 868.950 MHz (FREQ2=0x21, FREQ1=0x6B, FREQ0=0xD1 for 26 MHz CC1101 crystal)
 *    - Modulation: 2-FSK, 38.383 kBaud, 21.4 kHz frequency deviation
 *    - Sync Word: 0xD3 0x91 (30/32 bit correlation matching Verisure sensors)
 *    - Hardware SPI: Isolated ADC1 domain pins (GPIO 3, 5, 7, 9)
 *    - Owned by the radio task (radio_task.h): it reads the FIFO and queues the frames,
 *      loop() handles them
 *
 * 2. Cryptographic Security Engine (crypto.cpp):
 *    - AES-128-ECB hardware-accelerated decryption via mbedTLS
 *    - Key Derivation Function (KDF) from Master Key + Device ID
 *    - Message authentication ('VS' magic bytes) and Device ID echo check
 *    - 16-bit sequence counter anti-replay protection
 *
 * 3. Network & Integration Services:
 *    - Web Management Portal on Port 80 with Captive Portal DNS
 *    - MQTT Client with automatic reconnect & state publishing
 *    - Home Assistant MQTT Auto-Discovery for instantaneous integration
 *    - Serial CLI on USB-CDC (115200 baud)
 * ============================================================================
 */

#include <Arduino.h>
#include "crypto.h"
#include "config_manager.h"
#include "sensor_registry.h"
#include "web_portal.h"
#include "serial_cli.h"
#include "mqtt_manager.h"
#include "image_receiver.h"
#include "downlink.h"
#include "picture_token.h"
#include "duty_cycle.h"
#include "ha_discovery.h"
#include "gateway_diag.h"
#include "radio_task.h"
#ifdef RADIO_EVAL
#include "sniff_format.h"
#endif
#include <WiFi.h>
#include <math.h>

#include "hardware_pins.h"

static_assert(PIC_TOKEN_MAX_CAMERAS == DOWNLINK_MAX_CAMERAS,
              "picture token and downlink slots are per camera: keep equal");
static_assert(DUTY_MAX_CAMERAS == DOWNLINK_MAX_CAMERAS,
              "duty cycle and downlink slots are per camera: keep equal");

/** @brief CRC-16 as the CC1101 computes it (poly 0x8005, init 0xFFFF, no whitening). */
static uint16_t ccCrc16(const uint8_t* d, size_t n) {
    uint16_t crc = 0xFFFF;
    while (n--) {
        uint8_t b = *d++;
        for (int i = 0; i < 8; i++) {
            if (((crc & 0x8000) >> 8) ^ (b & 0x80)) crc = (crc << 1) ^ 0x8005;
            else crc <<= 1;
            b <<= 1;
        }
    }
    return crc;
}

static bool crcAt(const uint8_t* f, size_t n) {
    return ccCrc16(f, n) == (((uint16_t)f[n] << 8) | f[n + 1]);
}

unsigned long lastEventTime = 0;
unsigned long lastHeartbeat = 0;
unsigned long packetCount   = 0;

struct CalCandidate {
    bool     valid;
    uint8_t  id[8];
    int16_t  rssi;
    int8_t   freqEst;
    uint32_t startUs;
};
static CalCandidate s_lastMod0 = {};
static CalCandidate s_pendingCal = {};
static PchCalStats  s_calStats = {};

void pchCalReset() {
    memset(&s_calStats, 0, sizeof(s_calStats));
    s_lastMod0.valid = false;
    s_pendingCal.valid = false;
}

void pchCalGetStats(PchCalStats* out) {
    if (!out) return;
    *out = s_calStats;
    if (s_calStats.count > 0) {
        float avg = (float)s_calStats.freqEstSum / (float)s_calStats.count;
        long sug = (long)ConfigManager::get().freqOffset2 + lroundf(avg);
        if (sug < -128) sug = -128;
        if (sug > 127) sug = 127;
        out->suggestedOffset = (int8_t)sug;
    } else {
        out->suggestedOffset = ConfigManager::get().freqOffset2;
    }
}

static bool calMatchTime(uint32_t aUs, uint32_t bUs) {
    int32_t diff = (int32_t)(aUs - bUs);
    return diff >= -50000L && diff <= 50000L;
}

/**
 * @brief Print elapsed uptime timestamp and delta milliseconds to Serial monitor.
 * @param now Current uptime in milliseconds (millis()).
 */
void printTimestamp(unsigned long now) {
    unsigned long delta = (lastEventTime > 0) ? (now - lastEventTime) : 0;
    lastEventTime = now;
    Serial.printf("[%4lu.%03lu s] (+%5lu ms) ", now / 1000, now % 1000, delta);
}

static void calRecordPair(const uint8_t id[8], int16_t rssi1, int8_t fe1, int16_t rssi2, int8_t fe2) {
    s_calStats.count++;
    s_calStats.freqEstSum += fe2;
    s_calStats.lastRssi1 = rssi1;
    s_calStats.lastRssi2 = rssi2;
    s_calStats.lastFreqEst1 = fe1;
    s_calStats.lastFreqEst2 = fe2;

    char idStr[9];
    for (int i = 0; i < 8; i++) idStr[i] = isprint((unsigned char)id[i]) ? (char)id[i] : '?';
    idStr[8] = '\0';

    const float stepKhz = 26000.0f / 16384.0f;
    PchCalStats snap;
    pchCalGetStats(&snap);
    printTimestamp(millis());
    Serial.printf("[CAL] %s | #1 RSSI %d dBm freqEst %+d (%+.1f kHz) | #2 RSSI %d dBm freqEst %+d (%+.1f kHz) | n=%lu suggest=%d\n",
                  idStr, (int)rssi1, (int)fe1, fe1 * stepKhz,
                  (int)rssi2, (int)fe2, fe2 * stepKhz,
                  (unsigned long)snap.count, (int)snap.suggestedOffset);
}

/**
 * @brief Answer a camera poll (see downlink.h) with the alarm state and any queued
 *        picture command. Called right after the poll was authenticated, because the
 *        camera only listens for ~0.1 s.
 * @param frameStartUs radioNowUs() timestamp at which the poll frame started coming in
 *        (RadioFrame::startUs): the answer is skipped when it can no longer arrive in time.
 */
static void answerCameraPollNow(const VerisureTelemetry& t, uint32_t frameStartUs) {
    uint8_t key[16], plain[16], pkt[VERISURE_PACKET_LEN];
    uint8_t state = t.rawP1;             // plain[13] of the poll
    bool armed = downlinkArmed();

    // only polls with the persistent counter: the answer echoes it, which makes it unique
    if (!t.hasCounter) {
        Serial.printf("[Downlink] %s polls without the persistent counter: not answered (old camera firmware?)\n",
                      t.deviceId);
        return;
    }
    uint16_t first = 0;
    uint32_t map = 0;
    imageReceiverPollSeen(t.deviceId);   // may queue a resend for a picture that lost its end
    // Deadline guard: BEFORE the key derivation and downlinkCommandForPoll, so that a pending
    // resend/command is not consumed by an answer that is skipped (it stays queued for the next poll)
    const uint32_t ageUs = radioNowUs() - frameStartUs;
    if (ageUs > POLL_ANSWER_MAX_AGE_MS * 1000UL) {
        g_radioLoopDiag.pollLate++;
        Serial.printf("[Downlink] %s: poll answer skipped, poll frame is %lu ms old (limit %u ms)\n",
                      t.deviceId, (unsigned long)(ageUs / 1000), (unsigned)POLL_ANSWER_MAX_AGE_MS);
        return;
    }
    if (!cryptoDeriveDeviceKey(ConfigManager::get().masterKey, t.deviceId, key)) return;
    uint8_t cmd = downlinkCommandForPoll(t.deviceId, state, &first, &map);
    const bool advPch = (cmd != DOWNLINK_CMD_RESEND) && ConfigManager::get().pchEnabled &&
                        g_radioStatus2.online && !radioIsCalMode() && !imageReceiverPchSuppressed(t.deviceId);
    const uint8_t flags = (cmd != DOWNLINK_CMD_RESEND)
                        ? (uint8_t)(DL_FLAG_TOKEN | (advPch ? DL_FLAG_PCH : 0))
                        : 0;
    downlinkBuildPlain(t.counter, armed, cmd, first, map, plain, flags, DL_PCH_CH_869525);
    memcpy(pkt, t.deviceId, 8);
    bool ok = cryptoEncryptPayload(key, plain, &pkt[8]);
    memset(key, 0, sizeof(key));
    if (!ok) return;
    {
        const uint32_t sendAgeUs = radioNowUs() - frameStartUs;
        if (sendAgeUs > g_radioLoopDiag.maxPollAnswerAgeUs) g_radioLoopDiag.maxPollAnswerAgeUs = sendAgeUs;
    }
#ifdef RADIO_EVAL
    if (g_radioEvalPollDelayMs) delay(g_radioEvalPollDelayMs);   // CLI polldelay: provoke txLate
#endif
    if (!radioRequestTx(pkt, frameStartUs)) {
        Serial.printf("[Downlink] %s: poll answer not queued (radio command queue full)\n", t.deviceId);
        return;
    }
    if (cmd != DOWNLINK_CMD_RESEND) imageReceiverNotePollAnswer(t.deviceId, advPch);
    pictureTokenOnPollAnswered(t.deviceId, cmd == DOWNLINK_CMD_RESEND, (uint32_t)millis());
    g_radioLoopDiag.pollAnswers++;
    Serial.printf("[Downlink] -> %s: armed %d, cmd 0x%02X (poll %lu, camera state 0x%02X)",
                  t.deviceId, armed ? 1 : 0, cmd, (unsigned long)t.counter, state);
    if (cmd == DOWNLINK_CMD_RESEND)
        Serial.printf(", resend %d chunks from %u (map %08lX)", __builtin_popcount(map), first,
                      (unsigned long)map);
    else if (advPch)
        Serial.print(", pch");
    Serial.println();
    if (((state & DOWNLINK_STATE_ARMED) != 0) != armed) {
        Serial.printf("[Downlink] NOTE: %s reported armed %d before this answer\n",
                      t.deviceId, (state & DOWNLINK_STATE_ARMED) ? 1 : 0);
    }
}

/**
 * @brief answerCameraPollNow() followed by the MQTT publish of the picture this camera just
 *        finished: the (blocking) publish must not run before the poll answer is handed to the
 *        radio task, whatever way answerCameraPollNow() returns (answered, skipped, failed).
 */
static void answerCameraPoll(const VerisureTelemetry& t, uint32_t frameStartUs) {
    answerCameraPollNow(t, frameStartUs);
    imageReceiverAfterPollAnswer(t.deviceId);
}

/**
 * @brief Answer a camera 0xBB picture token request immediately within its RX window.
 *        Does not touch downlink commands or resend state (always cmd = 0).
 */
static void answerCameraTokenReqNow(const VerisureTelemetry& t, uint32_t frameStartUs) {
    uint8_t key[16], plain[16], pkt[VERISURE_PACKET_LEN];
    if (!t.hasCounter) {
        Serial.printf("[Token] %s token request without persistent counter: not answered\n", t.deviceId);
        return;
    }
    const uint32_t ageUs = radioNowUs() - frameStartUs;
    if (ageUs > POLL_ANSWER_MAX_AGE_MS * 1000UL) {
        g_radioLoopDiag.pollLate++;
        Serial.printf("[Token] %s: 0xBB answer skipped, frame is %lu ms old (limit %u ms)\n",
                      t.deviceId, (unsigned long)(ageUs / 1000), (unsigned)POLL_ANSWER_MAX_AGE_MS);
        return;
    }
    if (!cryptoDeriveDeviceKey(ConfigManager::get().masterKey, t.deviceId, key)) return;

    const uint32_t nowMs = (uint32_t)millis();
    PicTokenStatus before;
    pictureTokenGetStatus(nowMs, &before);
    const bool wasActiveHolder = (before.state == PIC_TOK_HELD &&
                                  strncmp(before.holderId, t.deviceId, 8) == 0);
    const bool granted = pictureTokenRequest(t.deviceId, nowMs);
    const bool advPch = ConfigManager::get().pchEnabled && g_radioStatus2.online &&
                        !radioIsCalMode() && !imageReceiverPchSuppressed(t.deviceId);
    const uint8_t flags = (uint8_t)(DL_FLAG_TOKEN |
                                    (advPch ? DL_FLAG_PCH : 0) |
                                    (granted ? DL_FLAG_GRANT : DL_FLAG_HOLD));
    const bool armed = downlinkArmed();
    downlinkBuildPlain(t.counter, armed, 0, 0, 0, plain, flags, DL_PCH_CH_869525);
    memcpy(pkt, t.deviceId, 8);
    bool ok = cryptoEncryptPayload(key, plain, &pkt[8]);
    memset(key, 0, sizeof(key));
    if (!ok) {
        if (granted && !wasActiveHolder) pictureTokenRelease(t.deviceId, nowMs);
        return;
    }
    {
        const uint32_t sendAgeUs = radioNowUs() - frameStartUs;
        if (sendAgeUs > g_radioLoopDiag.maxPollAnswerAgeUs) g_radioLoopDiag.maxPollAnswerAgeUs = sendAgeUs;
    }
#ifdef RADIO_EVAL
    if (g_radioEvalPollDelayMs) delay(g_radioEvalPollDelayMs);
#endif
    if (!radioRequestTx(pkt, frameStartUs)) {
        Serial.printf("[Token] %s: 0xBB answer not queued (radio command queue full)\n", t.deviceId);
        if (granted && !wasActiveHolder) pictureTokenRelease(t.deviceId, nowMs);
        return;
    }
    imageReceiverNotePollAnswer(t.deviceId, advPch);
    g_radioLoopDiag.pollAnswers++;
    Serial.printf("[Token] -> %s: %s (cmd 0x%02X, ctr %lu)%s\n",
                  t.deviceId, granted ? "GRANT" : "HOLD", t.rawP1, (unsigned long)t.counter,
                  advPch ? ", pch" : "");
}

static void answerCameraTokenReq(const VerisureTelemetry& t, uint32_t frameStartUs) {
    answerCameraTokenReqNow(t, frameStartUs);
    imageReceiverAfterPollAnswer(t.deviceId);
}

/**
 * @brief Answer a siren poll (0xBA) with the alarm state and any queued siren ON/OFF command.
 */
static void answerSirenPoll(const VerisureTelemetry& t, uint32_t frameStartUs) {
    uint8_t key[16], plain[16], pkt[VERISURE_PACKET_LEN];
    uint8_t state = t.rawP1;
    bool armed = downlinkArmed();

    if (!t.hasCounter) {
        Serial.printf("[Downlink] %s polls without the persistent counter: not answered\n",
                      t.deviceId);
        return;
    }
    const uint32_t ageUs = radioNowUs() - frameStartUs;
    if (ageUs > POLL_ANSWER_MAX_AGE_MS * 1000UL) {
        g_radioLoopDiag.pollLate++;
        Serial.printf("[Downlink] %s: siren poll answer skipped, frame is %lu ms old (limit %u ms)\n",
                      t.deviceId, (unsigned long)(ageUs / 1000), (unsigned)POLL_ANSWER_MAX_AGE_MS);
        return;
    }
    if (!cryptoDeriveDeviceKey(ConfigManager::get().masterKey, t.deviceId, key)) return;
    uint16_t first = 0;
    uint32_t map = 0;
    uint8_t cmd = downlinkCommandForPoll(t.deviceId, state, &first, &map);
    uint8_t dur = (uint8_t)(first & 0xFFU);
    uint8_t tone = (cmd == DOWNLINK_CMD_SIREN_ON) ? (uint8_t)(first >> 8) : 0;
    downlinkBuildPlain(t.counter, armed, cmd, dur, map, plain, 0, tone);
    memcpy(pkt, t.deviceId, 8);
    bool ok = cryptoEncryptPayload(key, plain, &pkt[8]);
    memset(key, 0, sizeof(key));
    if (!ok) return;
    {
        const uint32_t sendAgeUs = radioNowUs() - frameStartUs;
        if (sendAgeUs > g_radioLoopDiag.maxPollAnswerAgeUs) g_radioLoopDiag.maxPollAnswerAgeUs = sendAgeUs;
    }
    if (!radioRequestTx(pkt, frameStartUs)) {
        Serial.printf("[Downlink] %s: siren poll answer not queued (radio command queue full)\n", t.deviceId);
        return;
    }
    g_radioLoopDiag.pollAnswers++;
    Serial.printf("[Downlink] -> %s (siren): armed %d, cmd 0x%02X (dur %u s, tone %u, poll %lu, state 0x%02X)\n",
                  t.deviceId, armed ? 1 : 0, cmd, (unsigned)dur, (unsigned)tone, (unsigned long)t.counter, state);
}

#ifdef RADIO_EVAL
/** Evaluation only (CLI sniff): which frames are dumped as [Sniff#n] lines. Off at boot. */
volatile uint8_t g_sniffMode = SNIFF_OFF;

/**
 * Print-only dump of one frame (never touches crypto, replay, MQTT or downlink). Rate limited to
 * SNIFF_MAX_LINES_PER_SEC per second so a noisy band cannot stall loop(); the lines refused in a
 * window are reported once when the next window starts.
 */
static void sniffDump(const RadioFrame& item, bool crcBig, bool crcSmall, int rssiDbm, unsigned lqi,
                      float feKhz) {
    static SniffLimiter s_limiter;
    uint32_t suppressed;
    const bool allow = sniffAllow(s_limiter, (uint32_t)millis(), &suppressed);
    if (suppressed) Serial.printf("[Sniff] %lu line(s) suppressed (rate limit %d/s)\n",
                                  (unsigned long)suppressed, SNIFF_MAX_LINES_PER_SEC);
    if (!allow) return;
    // Over-the-air bytes only (the two status bytes after them are shown as rssi/lqi); the formatter
    // clamps again to sizeof(item.data).
    const size_t airLen = (item.len < RX_PKT_LEN) ? item.len : RX_PKT_LEN;
    char line[SNIFF_LINE_MAX];
    sniffFormatLine(line, sizeof(line), (unsigned)item.module + 1U, item.channel, item.data, airLen,
                    sizeof(item.data), crcBig, crcSmall, rssiDbm, lqi, feKhz);
    Serial.println(line);
}
#endif

/** Handles one item from the radio task's queue (a complete frame or a radio event). */
static void handleRadioFrame(const RadioFrame& item) {
    unsigned long now = millis();
    if (item.dropsBefore) {
        printTimestamp(now);
        Serial.printf("[Radio] %u item(s) lost to a full radio queue\n", item.dropsBefore);
    }
    const unsigned modNum = (unsigned)item.module + 1U;
    switch (item.kind) {
        case RADIO_ITEM_OVERFLOW:
            printTimestamp(now);
            Serial.printf("[Radio#%u] RX FIFO overflow (%u B lost), RX restarted\n", modNum, item.len);
            return;
        case RADIO_ITEM_PARTIAL:
            printTimestamp(now);
            Serial.printf("[Partial#%u %d B] dropped\n", modNum, item.len);
            return;
        case RADIO_ITEM_ONLINE:
            Serial.printf("\n[CC1101#%u] MODULE DETECTED! Version: 0x%02X. Radio initialized.\n", modNum, item.data[0]);
            return;
        case RADIO_ITEM_OFFLINE:
            printTimestamp(now);
            Serial.printf("[CC1101#%u] Module lost (SPI not responding), probing every 2 s.\n", modNum);
            return;
        case RADIO_ITEM_FRAME:
            break;
        default:
            return;
    }

    const uint8_t* frame = item.data;
    byte rawRssi = frame[RX_PKT_LEN];
    byte rawLqi  = frame[RX_PKT_LEN + 1];
    // Frequency offset the CC1101 estimated for this packet (FREQEST, two's complement,
    // f_xosc / 2^14 = 1.587 kHz per step), read at the start of the frame
    float freqErrKhz = item.freqEst * (26000.0f / 16384.0f);

    // Convert raw RSSI byte to dBm (CC1101 datasheet section 17.3)
    int packetRssi = (rawRssi >= 128) ? ((rawRssi - 256) / 2) - 74 : (rawRssi / 2) - 74;
    const uint8_t* activeMasterKey = ConfigManager::get().masterKey;

    bool crcBig = crcAt(frame, BIG_PACKET_LEN);
    bool crcSmall = crcAt(frame, VERISURE_PACKET_LEN);

    if (item.flags & RADIO_FLAG_CAL) {
        // Calibration frames are only measured against module #1; never pass to crypto/replay/MQTT/downlink
        if (crcBig || crcSmall) {
            if (s_lastMod0.valid && calMatchTime(item.startUs, s_lastMod0.startUs) &&
                memcmp(s_lastMod0.id, frame, 8) == 0) {
                calRecordPair(frame, s_lastMod0.rssi, s_lastMod0.freqEst, (int16_t)packetRssi, item.freqEst);
                s_lastMod0.valid = false;
            } else {
                s_pendingCal.valid = true;
                memcpy(s_pendingCal.id, frame, 8);
                s_pendingCal.rssi = (int16_t)packetRssi;
                s_pendingCal.freqEst = item.freqEst;
                s_pendingCal.startUs = item.startUs;
            }
        }
        return;
    }

    if ((crcBig || crcSmall) && item.module == 0) {
        if (s_pendingCal.valid && calMatchTime(item.startUs, s_pendingCal.startUs) &&
            memcmp(s_pendingCal.id, frame, 8) == 0) {
            calRecordPair(frame, (int16_t)packetRssi, item.freqEst, s_pendingCal.rssi, s_pendingCal.freqEst);
            s_pendingCal.valid = false;
        } else {
            s_lastMod0.valid = true;
            memcpy(s_lastMod0.id, frame, 8);
            s_lastMod0.rssi = (int16_t)packetRssi;
            s_lastMod0.freqEst = item.freqEst;
            s_lastMod0.startUs = item.startUs;
        }
    }

    if (crcBig || (item.module == 0 && crcSmall)) g_gatewayDiag.rxCrcOk++;

    if (crcBig) {
        if (!cryptoProcessBigPacket(frame, activeMasterKey, item.channel)) {
#ifdef RADIO_EVAL
            if (g_sniffMode == SNIFF_UNKNOWN) sniffDump(item, crcBig, crcSmall, packetRssi, rawLqi & 0x7F, freqErrKhz);
#endif
            printTimestamp(now);
            Serial.printf("[Security] Module #%u big packet rejected (invalid key or type).\n", modNum);
        }
    } else if (item.module == 0 && crcSmall) {
        VerisureTelemetry telem;
        if (cryptoProcessPacket(frame, packetRssi, rawLqi & 0x7F, activeMasterKey, &telem)) {
            if (haIsMotionCameraSensor(telem.deviceId)) {
                const uint32_t tokNow = (uint32_t)now;
                dutyCycleRecordCamPkt24(telem.deviceId, tokNow);
                if (telem.rawP0 == DOWNLINK_POLL_MARKER) {
                    answerCameraPoll(telem, item.startUs);
                    // a poll always counts, also 0 = unknown (the camera restarted)
                    telem.batteryLoadMv = downlinkLoadMv(telem.rawP1);
                } else {
                    if (telem.rawP0 == PIC_TOKEN_REQ_MARKER) {
                        answerCameraTokenReq(telem, item.startUs);
                    } else if (telem.rawP0 == PIC_TOKEN_REL_MARKER) {
                        if (pictureTokenRelease(telem.deviceId, tokNow)) {
                            printTimestamp(now);
                            Serial.printf("[Token] Released by %s (reason 0x%02X)\n", telem.deviceId, telem.rawP1);
                        }
                    } else if (telem.rawP0 == 0xB2) {
                        pictureTokenOnPicStart(telem.deviceId, tokNow);
                        imageReceiverNotePicStart(telem.deviceId);
                    } else if (telem.rawP0 == 0x00 || telem.rawP0 == 0xB8 || telem.rawP0 == 0xB3) {
                        pictureTokenRelease(telem.deviceId, tokNow);
                    }
                    // other packets keep the last value the camera reported
                    const SensorRecord* prev = sensorRegistryFind(telem.deviceId);
                    if (prev) telem.batteryLoadMv = prev->lastTelem.batteryLoadMv;
                }
            } else if (haIsSirenSensor(telem.deviceId) && telem.rawP0 == DOWNLINK_POLL_MARKER) {
                answerSirenPoll(telem, item.startUs);
            }
            if (telem.rawP0 == 0xB6 && telem.rawP1 != 0xFF) {
                telem.hasTemp = true;
                telem.tempC = (telem.rawP1 * 0.5f) - 40.0f;
            } else {
                const SensorRecord* prev = sensorRegistryFind(telem.deviceId);
                if (prev && prev->hasTemp) {
                    telem.hasTemp = true;
                    telem.tempC = prev->tempC;
                }
            }
            packetCount++;
            sensorRegistryUpdate(telem); // Cache in memory
            mqttPublishTelemetry(telem); // Publish to Home Assistant MQTT
            printTimestamp(now);
            Serial.printf(">>> [#%04lu] [AUTHENTICATED] Device: \"%s\" | Seq: #%lu <<<\n",
                          packetCount, telem.deviceId, (unsigned long)telem.counter);
            Serial.printf("    Magnet  : %s\n", telem.isOpen ? "OPEN [ALARM!]" : "CLOSED (Secure)");
            Serial.printf("    Shock   : %s\n", telem.isShock ? "SHOCK / VIBRATION DETECTED!" : "Clear");
            Serial.printf("    Tamper  : %s\n", telem.isButton ? "TAMPER (Cover removed!)" : "Secure (Cover closed)");
            Serial.printf("    Battery : %u mV (%u%%)%s", telem.batteryMv, telem.batteryPct, telem.isLowBatt ? " [LOW BATTERY!]" : "");
            if (telem.batteryLoadMv) Serial.printf(" | under load %u mV", telem.batteryLoadMv);
            Serial.println();
            Serial.printf("    Signal  : RSSI %d dBm | LQI %u | Freq. error %+.1f kHz | CRC: OK\n",
                          telem.rssi, telem.lqi, freqErrKhz);
            Serial.printf("    Pins    : P0=0x%02X P1=0x%02X P2=0x%02X (P2_0=%d) RST_CAUSE=%u\n\n",
                          telem.rawP0, telem.rawP1, telem.rawP2, telem.rawP2 & 0x01, telem.rawRst);
        } else {
#ifdef RADIO_EVAL
            if (g_sniffMode == SNIFF_UNKNOWN) sniffDump(item, crcBig, crcSmall, packetRssi, rawLqi & 0x7F, freqErrKhz);
#endif
            printTimestamp(now);
            Serial.println("[Security] Packet received but rejected (invalid key, spoofing, or replay attack).");
        }
    } else {
#ifdef RADIO_EVAL
        if (g_sniffMode == SNIFF_UNKNOWN) sniffDump(item, crcBig, crcSmall, packetRssi, rawLqi & 0x7F, freqErrKhz);
#endif
        printTimestamp(now);
        Serial.printf("[Radio#%u] Packet received with invalid CRC (dropped): RSSI %d dBm, freq. error %+.1f kHz%s\n",
                      modNum, packetRssi, freqErrKhz,
                      (item.flags & RADIO_FLAG_TX_OVERLAP) ? " [TX_OVERLAP]" : "");
    }
#ifdef RADIO_EVAL
    // 'sniff all': after every branch above, so it never delays a poll answer (95 ms deadline).
    // The only early return after the frame kind switch is the CAL block (CAL frames are not
    // dumped); keep it that way, or this line is skipped.
    if (g_sniffMode == SNIFF_ALL) sniffDump(item, crcBig, crcSmall, packetRssi, rawLqi & 0x7F, freqErrKhz);
#endif
}

/**
 * Handles everything the radio task has queued, in arrival order. The first wait may block up to
 * waitMs (it replaces loop()'s old delay(2)), the rest is polled. Bounded to 64 items per
 * call (the queue holds RADIO_QUEUE_DEPTH) so that the other services still run if frames come
 * in faster than they are handled; the rest is taken by the next drain. The loop watchdog is fed per
 * item: handling one can block (USB serial prints, MQTT publish), 64 of them can take longer than 5 s.
 * Only loop() calls this (an OTA upload does not: it must not block on prints or MQTT).
 */
static void radioDrainQueue(uint32_t waitMs) {
    static unsigned long s_lastDrainMs = 0;
    unsigned long nowMs = millis();
    if (s_lastDrainMs) {
        uint32_t gapMs = (uint32_t)(nowMs - s_lastDrainMs);
        if (gapMs > g_radioLoopDiag.maxDrainGapMs) g_radioLoopDiag.maxDrainGapMs = gapMs;
    }
    s_lastDrainMs = nowMs;
    RadioFrame item;
    for (int n = 0; n < 64 && radioReceive(&item, n == 0 ? waitMs : 0); n++) {
        feedLoopWDT();   // before the item: a poll answer, a publish and the prints follow
        handleRadioFrame(item);
    }
    feedLoopWDT();
}

void setup() {
    Serial.begin(115200);
    // Serial over USB CDC blocks a write for up to tx_timeout_ms (default 250 ms) while the host
    // holds the port open without reading, ~10 writes per packet. 20 ms still fits a normal burst
    // (the CDC FIFO is 64 B and drains at 64 B per USB frame) and bounds a stalled host. Not 0:
    // that would drop the tail of every burst longer than 64 B, e.g. the CLI 'status' output.
    Serial.setTxTimeoutMs(20);
    delay(2000);

    Serial.println("\n=======================================================");
    Serial.println("  ESP32-S2 VERISURE GATEWAY & RECEIVER (868.950 MHz)   ");
    Serial.println("=======================================================\n");
    // a watchdog reboot of a hung loop() shows up here as "task_wdt" (also in 'status' and /api/status)
    Serial.printf("[System] Last reset reason: %s\n", gatewayDiagSample().resetReason);

    // 1. Initialize WebPortal, NVS Config, Serial CLI & MQTT
    webPortalInit();
    downlinkInit();
    pictureTokenInit();
    dutyCycleInit();
    imageReceiverInit();   // prints the PSRAM size; the picture buffers follow with each camera's first picture
    serialCliInit();
    mqttInit();

    Serial.println("[System] Allowing Wi-Fi time to establish connection before radio start...");
    unsigned long wifiWaitStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiWaitStart < 8000) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[Wi-Fi] Connected! IP: %s | RSSI: %d dBm\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
        Serial.println("[Wi-Fi] Connecting in background...");
    }

    // 2. Initialize both CC1101 transceivers on ADC1 pins (radio_task.cpp), then hand the
    //    SPI bus over to the radio task: from radioTaskStart() on nothing else touches it.
    uint8_t ver1 = 0, ver2 = 0;
    bool online2 = false;
    bool online1 = radioInit(&ver1, &ver2, &online2);

    if (online1) {
        Serial.printf("  [CC1101#1] SPI OK! Chip Version: 0x%02X (SCK=%d, MISO=%d, MOSI=%d, CS=%d)\n",
                      ver1, PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
        printTimestamp(millis());
        Serial.println("[System] Module #1 listening on 868.950 MHz (Robust RX, 30/32 Sync)...");
    } else {
        Serial.printf("  [CC1101#1] No communication (read 0x%02X on SCK=%d, MISO=%d, MOSI=%d, CS=%d).\n",
                      ver1, PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
        Serial.println("  [CC1101#1] Disconnected - probing every 2 s.");
    }
    if (online2) {
        Serial.printf("  [CC1101#2] SPI OK! Chip Version: 0x%02X (CS2=%d, 869.525 MHz PCH, pch=%s)\n",
                      ver2, PIN_CS2, ConfigManager::get().pchEnabled ? "on" : "off");
    } else {
        Serial.printf("  [CC1101#2] Offline / not connected (read 0x%02X on CS2=%d, probing every 2 s).\n",
                      ver2, PIN_CS2);
    }
    if (!radioTaskStart(online1, online2)) {
        // No fallback to a polling radio in loop(): that code is gone, and a loop() that cannot
        // keep up is what the task fixes. The gateway stays up (web, MQTT, CLI) without reception.
        Serial.println("[System] ERROR: radio task/PSRAM queue could not be created, NO radio reception! Check 'status' and the board's PSRAM.");
    }

    lastEventTime = millis();
    lastHeartbeat = lastEventTime;

    // Subscribe loopTask to the task watchdog (5 s, panic = reboot, reset reason task_wdt). Bench
    // 2026-11-20: loop() hung once after a 2 h soak (HTTP, MQTT and USB CDC dead, ping alive) and
    // nothing rebooted the gateway. The core feeds the watchdog once per loop() iteration; every
    // longer blocking path (OTA upload, MQTT connect/picture publish, CLI scan/connect) feeds it
    // itself with feedLoopWDT(). Enabled last: setup() above blocks for ~10 s (delays, Wi-Fi wait).
    enableLoopWDT();
}

void loop() {
    unsigned long now = millis();

    // Heartbeat every 2 seconds (RSSI/MARCSTATE come from the radio task's cache: no SPI here)
    if (now - lastHeartbeat >= 2000) {
        lastHeartbeat = now;
        const bool on1 = g_radioStatus.online;
        const bool on2 = g_radioStatus2.online;
        const char* m2Str = !on2 ? "Offline" : (radioIsCalMode() ? "CAL 868.95 MHz" : "PCH 869.525 MHz");
        Serial.printf("[Gateway Heartbeat] Wi-Fi: %s | MQTT: %s | CC1101#1: %s (%d dBm) | CC1101#2: %s (%d dBm) | Packets: %lu\n",
                      WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "Connecting...",
                      mqttGetStatusString().c_str(),
                      on1 ? "Online 868.95 MHz" : "Disconnected",
                      on1 ? (int)g_radioStatus.rssiDbm : 0,
                      m2Str,
                      on2 ? (int)g_radioStatus2.rssiDbm : 0,
                      packetCount);
    }
    const int cachedRssi = g_radioStatus.rssiDbm;
    const byte cachedMarc = g_radioStatus.marcState;

    // 1. Process Background Services (Non-blocking). The radio task keeps reading the FIFO
    //    meanwhile; the queue is drained between the services.
    radioDrainQueue(0);
    webPortalLoop(cachedRssi, cachedMarc, packetCount);
    radioDrainQueue(0);
    serialCliLoop();
    mqttLoop(cachedRssi, cachedMarc, packetCount);
    // mandatory drain: imageReceiverLoop closes a picture after JPEG_QUIET_MS without a chunk,
    // which must not happen while chunks are still queued
    radioDrainQueue(0);
    imageReceiverLoop();
    {
        char expId[9];
        if (pictureTokenTick((uint32_t)millis(), expId, sizeof(expId))) {
            printTimestamp(millis());
            Serial.printf("[Token] Lease expired for %s\n", expId);
        }
    }
    {
        static uint32_t s_lastGwTxCount = 0;
        const uint32_t curTx = g_radioStatus.txCount;
        const uint32_t delta = (uint32_t)(curTx - s_lastGwTxCount);
        const uint32_t dutyNow = (uint32_t)millis();
        if (delta > 0) {
            for (uint32_t i = 0; i < delta; i++) {
                dutyCycleRecordGwTx(dutyNow);
            }
            s_lastGwTxCount = curTx;
        } else {
            dutyCycleTick(dutyNow);
        }
    }
    cryptoReplayFlush();

    // 2. Wait for the next frame (replaces the old delay(2))
    radioDrainQueue(2);
}
