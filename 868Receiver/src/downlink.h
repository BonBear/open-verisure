/**
 * @file downlink.h
 * @brief Gateway -> camera downlink: alarm state, picture requests and resend requests.
 *
 * The camera (MD_CAM CC1110, cam_alarm_loop) sends a poll packet every ~30 s (disarmed and
 * idle: every ~120 s): an ordinary telemetry packet in the persistent-counter format (flag
 * 0x20) with plain[12] = 0xBA and plain[13] = camera state (bit 0 armed, bit 1 Blackfin
 * powered, bit 2 a downlink command arrived, bits 7:3 battery voltage under load: 0 unknown,
 * else 2275 + 25 * value mV). After the poll it listens for ~0.1 s.
 *
 * The gateway answers only a poll that passed authentication and the replay filter, with a
 * 24-byte packet: 8 bytes device ID (plain) + AES-128-ECB(device key, plaintext):
 *   [0] 'D'  [1] 'L'  [2..5] the poll's 32-bit counter (big endian)  [6] armed (0/1)
 *   [7] command (0 none, 0xD1..0xD6 take a picture, 0xE8 HOLD, 0xF0 resend)
 *   [8..9] resend: first chunk (big endian)  [10..13] resend: bitmap, bit i (byte 10 + i/8,
 *   bit i%8) = chunk first + i is missing  [14] 'V'  [15] 'S'
 * The camera accepts it only if the counter is the one of its own last poll. The counter
 * never repeats (it survives resets, see crypto.cpp), so a recorded answer never fits again.
 */
#pragma once

#include <Arduino.h>

#define DOWNLINK_POLL_MARKER  0xBA
#define DOWNLINK_STATE_ARMED  0x01
#define DOWNLINK_STATE_BF_ON  0x02
#define DOWNLINK_STATE_ACK    0x04
#define DOWNLINK_STATE_LOAD_SHIFT 3   // bits 7:3: battery under load, see above

/** Battery voltage under load from a poll's state byte, in mV (0 = unknown). */
static inline uint16_t downlinkLoadMv(uint8_t state) {
    uint8_t v = state >> DOWNLINK_STATE_LOAD_SHIFT;
    return v ? 2275 + 25 * v : 0;
}

/** Load the stored alarm state from NVS. */
void downlinkInit();

/** Alarm state sent to the cameras. */
bool downlinkArmed();

/** Set and store the alarm state (from MQTT or the serial console). */
void downlinkSetArmed(bool armed);

/** Queue a picture command (0xD1 full, 0xD2 640x480, 0xD3 delta, 0xD4 grayscale with flash, 0xD5/0xD6 as 0xD3/0xD2 but flash in the dark) for a camera; sent with its next poll answer.
 *  Returns false for an unknown command or a full table. */
bool downlinkRequestPicture(const char* deviceId, uint8_t cmd);

#define DOWNLINK_CMD_RESEND   0xF0

/** Ask a camera to resend the picture chunks first + i for every bit i set in map (32 chunks
 *  at most per request) with its next poll answer (sent once, a picture command goes first). */
bool downlinkRequestResend(const char* deviceId, uint16_t first, uint32_t map);

/** True while a resend request for this camera waits for its next poll answer. */
bool downlinkResendPending(const char* deviceId);

/** Command to put in the answer to this poll (0 = none) and, for DOWNLINK_CMD_RESEND, its first
 *  chunk and bitmap. A pending picture command stays queued until the camera reports
 *  DOWNLINK_STATE_ACK in a later poll; a resend request goes out once. */
uint8_t downlinkCommandForPoll(const char* deviceId, uint8_t cameraState, uint16_t* first, uint32_t* map);

/** Build the 16-byte plaintext of a poll answer (see the format above). */
void downlinkBuildPlain(uint32_t pollCounter, bool armed, uint8_t cmd, uint16_t first, uint32_t map,
                        uint8_t out[16]);
