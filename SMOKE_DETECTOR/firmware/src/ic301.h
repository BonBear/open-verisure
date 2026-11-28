/*
 * Command UART to the IC301 sound/chime chip. EFR32 PB12 (pin 32, USART0 TX location 7) ->
 * IC301 pin 30: 115200 baud, 8 data bits, odd parity, 1 stop bit, idle high. EFR32 PA4 (pin 29)
 * <- IC301 pin 22 (input only, hypothesis: busy, active low while a sound plays).
 *
 * Frame (stock firmware capture 2026-11-28): 0xFF wake byte, ~6 ms gap, 0xFE sync byte, ~0.8 ms
 * gap, then the command bytes back to back. 0x81 as "play sound" and PA4 as "busy" are
 * hypotheses (SMOKE_DETECTOR/docs/re_worksheet.md section 7b).
 *
 * The USART route is only enabled while a frame is sent; between frames the GPIO (DOUT = 1)
 * holds PB12 high, also in EM2.
 */
#ifndef IC301_H
#define IC301_H

#include <stdbool.h>
#include <stdint.h>

/* Call after clocks_init (the USART needs the HF clock). PB12 idles high, PA4 is an input.
 * radio_init must have run before the first ic301_send (waits use radio_time_us). */
void ic301_init(void);

/* Sends one frame (wake, sync, cmd[0..len-1]). Blocks about 7 ms plus the command bytes: call
 * from the main loop only, never from an ISR. Returns false for len == 0, cmd == NULL or when a
 * wait timed out (the route is disabled again in every case). */
bool ic301_send(const uint8_t *cmd, uint8_t len);

/* Sends {0x81, a, b} (hypothesis: play sound a, b). */
bool ic301_play(uint8_t a, uint8_t b);

/* True while PA4 is low. */
bool ic301_busy(void);

/* After a command: waits up to ~20 ms for PA4 to go low, then until it is high again or
 * timeout_ms has passed. Returns the ms PA4 was low (0 if it never went low). */
uint32_t ic301_wait_idle_ms(uint32_t timeout_ms);

#endif /* IC301_H */
