/*
 * OpenVerisure 24-byte uplink packet (same format as the door sensor, see
 * documentation/cryptography_and_protocol.md) plus the CC1101-style CRC-16.
 */
#ifndef PACKET_H
#define PACKET_H

#include <stdbool.h>
#include <stdint.h>

#define PKT_ID_LEN        8u
#define PKT_LEN           24u            /* 8-byte plaintext ID + one AES block */
#define PKT_AIR_LEN       (PKT_LEN + 2u) /* + CRC-16, as the PHY sends fixed 26 bytes */

#define PKT_FLAG_ALARM    0x01u  /* bit 0: alarm active (door: reed open) */
#define PKT_FLAG_TAMPER   0x04u
#define PKT_FLAG_BOOT     0x08u  /* first packet after power-on */
#define PKT_FLAG_LOW_BATT 0x10u
#define PKT_FLAG_CTR32    0x20u  /* 32-bit counter, high half in plain[14..15] */
/* flags bit 1 (0x02) is the door shock flag: it must stay 0 here. */

/* plain[12] packet type of the smoke detector. The gateway tells smoke from door sensors by
 * these values (door firmware cannot produce them: bits 0x14 are driven low before sampling).
 * Every smoke packet type must stay within 0x5D..0x5F: the gateway classifies per packet. */
#define PKT_TYPE_SMOKE_STATUS   0x5Du  /* plain[13] = PKT_SMOKE_* bits */
#define PKT_TYPE_SMOKE_CLIMATE  0x5Eu  /* plain[13] = temperature, raw*0.5-40 C, 0xFF = none (reserved) */
#define PKT_TYPE_SMOKE_HUMIDITY 0x5Fu  /* plain[13] = RH 0..100 %, 0xFF = none (reserved) */

/* plain[13] status bits of PKT_TYPE_SMOKE_STATUS. */
#define PKT_SMOKE_FAULT     0x01u  /* chamber fault */
#define PKT_SMOKE_DIRTY     0x02u  /* chamber dirty / maintenance */
#define PKT_SMOKE_TEST      0x04u  /* test button pressed */
#define PKT_SMOKE_HEARTBEAT 0x08u  /* periodic timer packet (0 = event packet) */
#define PKT_SMOKE_HUSHED    0x10u  /* alarm hushed */

/* Reset cause in flags bits 7:6, same codes as the CC1110 devices. */
#define PKT_RST_POWER_ON  0u
#define PKT_RST_PIN       1u
#define PKT_RST_WATCHDOG  2u
#define PKT_RST_OTHER     3u

typedef struct {
  uint32_t counter;
  uint8_t  flags;        /* PKT_FLAG_* without PKT_FLAG_CTR32 and reset cause */
  uint8_t  rst_cause;    /* PKT_RST_* */
  uint8_t  batt_pct;
  uint16_t batt_mv;
  uint8_t  diag0;        /* plain[12]: packet type / diagnostic */
  uint8_t  diag1;        /* plain[13] */
} pkt_fields_t;

/* Fills the 16-byte plaintext block for device_id (8 characters). */
void pkt_build_plain(const char *device_id, const pkt_fields_t *f, uint8_t plain[16]);

/* CRC-16 as the CC1101 computes it: poly 0x8005, init 0xFFFF, MSB first, no reflection. */
uint16_t pkt_crc16(const uint8_t *data, uint32_t len);

/* Builds the on-air frame: ID, ciphertext and CRC-16 (big-endian). */
void pkt_build_frame(const char *device_id, const uint8_t cipher[16], uint8_t frame[PKT_AIR_LEN]);

#endif /* PACKET_H */
