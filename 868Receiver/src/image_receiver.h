/**
 * @file image_receiver.h
 * @brief RF Camera Image Chunk Reassembly for Verisure Sub-1GHz Sensor
 */

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stdbool.h>

#define MAX_IMAGE_SIZE 32768

void imageReceiverInit(void);

/**
 * @brief Process an incoming authenticated image chunk from RF.
 * @param deviceId 8-char sensor ID.
 * @param chunkIdx 16-bit chunk index (0..N).
 * @param chunkLen Number of bytes in this chunk (1..8).
 * @param isFirst True if this is the first chunk (SOI).
 * @param isLast True if this is the final chunk (EOI).
 * @param data Pointer to chunkLen bytes of JPEG data.
 */
void imageReceiverProcessChunk(const char* deviceId, uint16_t chunkIdx, uint8_t chunkLen,
                               bool isFirst, bool isLast, const uint8_t* data);
