/**
 * @file downlink.h
 * @brief Gateway -> camera downlink: alarm state and picture requests.
 *
 * The camera (MD_CAM CC1110, cam_alarm_loop) sends a poll packet every ~30 s: an ordinary
 * telemetry packet with plain[12] = 0xBA, plain[13..14] = a challenge that is new for every
 * poll and plain[15] = camera state (bit 0 armed, bit 1 Blackfin powered, bit 2 a downlink
 * command arrived). After the poll it listens for ~0.1 s.
 *
 * The gateway answers only a poll that passed authentication and the replay filter, with a
 * 24-byte packet: 8 bytes device ID (plain) + AES-128-ECB(device key, plaintext):
 *   [0] 'D'  [1] 'L'  [2..3] poll sequence  [4..5] poll challenge  [6] armed (0/1)
 *   [7] command (0 none, 0xD1..0xD3 take a picture)  [8..11] device ID[0..3]
 *   [12..13] 0  [14] 'V'  [15] 'S'
 * The camera accepts it only if the sequence and challenge match its own last poll, so a
 * recorded answer fits a later poll only by chance (1 in 65536).
 */
#pragma once

#include <Arduino.h>

#define DOWNLINK_POLL_MARKER  0xBA
#define DOWNLINK_STATE_ARMED  0x01
#define DOWNLINK_STATE_BF_ON  0x02
#define DOWNLINK_STATE_ACK    0x04

/** Load the stored alarm state from NVS. */
void downlinkInit();

/** Alarm state sent to the cameras. */
bool downlinkArmed();

/** Set and store the alarm state (from MQTT or the serial console). */
void downlinkSetArmed(bool armed);

/** Queue a picture command (0xD1..0xD3) for a camera; sent with its next poll answer.
 *  Returns false for an unknown command or a full table. */
bool downlinkRequestPicture(const char* deviceId, uint8_t cmd);

/** Command to put in the answer to this poll (0 = none). A pending command stays queued
 *  until the camera reports DOWNLINK_STATE_ACK in a later poll. */
uint8_t downlinkCommandForPoll(const char* deviceId, uint8_t cameraState);

/** Build the 16-byte plaintext of a poll answer (see the format above). */
void downlinkBuildPlain(uint16_t pollSeq, uint16_t nonce, bool armed, uint8_t cmd,
                        const char* deviceId, uint8_t out[16]);
