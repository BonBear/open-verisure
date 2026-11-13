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

/* ---------------------------------------------------------------------------------------
 * Raw grayscale tiles (packet type 0x81) from the MD CAM camera link (BF_IMG_LINK).
 * Tile n carries 8 pixels of a RAW_IMG_W x RAW_IMG_H 8-bit picture, pixels 8n .. 8n+7.
 * Tiles are placed by number (a lost packet only leaves a gap) and the picture is kept
 * as an 8-bit grayscale BMP, published on MQTT when complete or after a quiet period,
 * and served on /api/camera.bmp.
 * ------------------------------------------------------------------------------------- */
#define RAW_IMG_W 320
#define RAW_IMG_H 240
#define RAW_TILE_PIX 8
#define RAW_TILES ((RAW_IMG_W * RAW_IMG_H) / RAW_TILE_PIX)

void imageReceiverProcessTile(const char* deviceId, uint16_t tile, const uint8_t* pix8);

/** Call from loop(): publishes the raw picture on MQTT when due. */
void imageReceiverLoop(void);

/** Current raw picture as BMP (nullptr if none yet). */
const uint8_t* imageReceiverBmp(size_t* len);

/** Tiles received for the current raw picture. */
uint16_t imageReceiverTilesHave(void);
