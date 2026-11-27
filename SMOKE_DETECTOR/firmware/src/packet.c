#include "packet.h"

void pkt_build_plain(const char *device_id, const pkt_fields_t *f, uint8_t plain[16])
{
  plain[0]  = (uint8_t)(f->counter >> 8);
  plain[1]  = (uint8_t)f->counter;
  plain[2]  = (uint8_t)((f->flags & 0x1Fu) | PKT_FLAG_CTR32 | ((f->rst_cause & 0x3u) << 6));
  plain[3]  = f->batt_pct;
  plain[4]  = (uint8_t)(f->batt_mv >> 8);
  plain[5]  = (uint8_t)f->batt_mv;
  plain[6]  = (uint8_t)device_id[0];   /* ID echo, checked by the gateway */
  plain[7]  = (uint8_t)device_id[1];
  plain[8]  = (uint8_t)device_id[2];
  plain[9]  = (uint8_t)device_id[3];
  plain[10] = 'V';                     /* magic */
  plain[11] = 'S';
  plain[12] = f->diag0;
  plain[13] = f->diag1;
  plain[14] = (uint8_t)(f->counter >> 16);
  plain[15] = (uint8_t)(f->counter >> 24);
}

uint16_t pkt_crc16(const uint8_t *data, uint32_t len)
{
  uint16_t crc = 0xFFFFu;

  while (len--) {
    uint8_t b = *data++;
    for (uint8_t i = 0; i < 8u; i++) {
      if (((crc & 0x8000u) >> 8) ^ (b & 0x80u)) {
        crc = (uint16_t)((crc << 1) ^ 0x8005u);
      } else {
        crc = (uint16_t)(crc << 1);
      }
      b = (uint8_t)(b << 1);
    }
  }
  return crc;
}

void pkt_build_frame(const char *device_id, const uint8_t cipher[16], uint8_t frame[PKT_AIR_LEN])
{
  for (uint32_t i = 0; i < PKT_ID_LEN; i++) {
    frame[i] = (uint8_t)device_id[i];
  }
  for (uint32_t i = 0; i < 16u; i++) {
    frame[PKT_ID_LEN + i] = cipher[i];
  }
  const uint16_t crc = pkt_crc16(frame, PKT_LEN);
  frame[PKT_LEN]      = (uint8_t)(crc >> 8);
  frame[PKT_LEN + 1u] = (uint8_t)crc;
}
