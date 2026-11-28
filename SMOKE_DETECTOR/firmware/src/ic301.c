/*
 * IC301 command UART on USART0 (TX only, PB12 = location 7). See ic301.h for the frame format.
 * Waits use the RAIL microsecond timer (radio_time_us), like delay_ms in main.c; the DWT cycle
 * counter is intentionally not used.
 */
#include "ic301.h"

#include <stddef.h>

#include "em_cmu.h"
#include "em_device.h"
#include "em_gpio.h"
#include "em_usart.h"

#include "radio.h"

#define IC301_TX_PORT      gpioPortB
#define IC301_TX_PIN       12u
#define IC301_BUSY_PORT    gpioPortA
#define IC301_BUSY_PIN     4u

#define IC301_WAKE_BYTE    0xFFu
#define IC301_SYNC_BYTE    0xFEu
#define IC301_WAKE_GAP_US  6000u
#define IC301_SYNC_GAP_US  800u
#define IC301_BYTE_TMO_US  2000u    /* one byte takes ~113 us; generous bound */
#define IC301_PLAY_CMD     0x81u
#define IC301_START_TMO_MS 20u      /* PA4 goes low within this time after a command */

static bool wait_flag(uint32_t flag)
{
  const uint32_t start = radio_time_us();
  while ((USART_StatusGet(USART0) & flag) == 0u) {
    if ((radio_time_us() - start) > IC301_BYTE_TMO_US) {
      return false;
    }
  }
  return true;
}

static void wait_us(uint32_t us)
{
  const uint32_t start = radio_time_us();
  while ((radio_time_us() - start) < us) {
  }
}

static bool tx_byte(uint8_t b)
{
  if (!wait_flag(USART_STATUS_TXBL)) {
    return false;
  }
  USART0->TXDATA = b;
  return true;
}

static bool tx_byte_wait(uint8_t b)
{
  return tx_byte(b) && wait_flag(USART_STATUS_TXC);
}

void ic301_init(void)
{
  CMU_ClockEnable(cmuClock_GPIO, true);
  CMU_ClockEnable(cmuClock_USART0, true);

  GPIO_PinModeSet(IC301_TX_PORT, IC301_TX_PIN, gpioModePushPull, 1);   /* idle high */
  GPIO_PinModeSet(IC301_BUSY_PORT, IC301_BUSY_PIN, gpioModeInput, 0);

  USART_InitAsync_TypeDef init = USART_INITASYNC_DEFAULT;
  init.enable = usartEnableTx;
  init.baudrate = 115200u;
  init.databits = usartDatabits8;
  init.parity = usartOddParity;
  init.stopbits = usartStopbits1;
  USART_InitAsync(USART0, &init);

  /* Route stays off (ROUTEPEN = 0): the GPIO holds the line high until a frame is sent. */
  USART0->ROUTELOC0 = (USART0->ROUTELOC0 & ~_USART_ROUTELOC0_TXLOC_MASK) |
                      USART_ROUTELOC0_TXLOC_LOC7;
  USART0->ROUTEPEN &= ~USART_ROUTEPEN_TXPEN;
}

bool ic301_send(const uint8_t *cmd, uint8_t len)
{
  if (cmd == NULL || len == 0u) {
    return false;
  }

  USART0->ROUTEPEN |= USART_ROUTEPEN_TXPEN;      /* USART idles high: no glitch on the line */
  bool ok = tx_byte_wait(IC301_WAKE_BYTE);
  if (ok) {
    wait_us(IC301_WAKE_GAP_US);
    ok = tx_byte_wait(IC301_SYNC_BYTE);
  }
  if (ok) {
    wait_us(IC301_SYNC_GAP_US);
    for (uint8_t i = 0u; ok && i < len; i++) {
      ok = tx_byte(cmd[i]);
    }
    ok = ok && wait_flag(USART_STATUS_TXC);
  }
  if (!ok) {
    (void)wait_flag(USART_STATUS_TXC);           /* no truncated byte after a timeout */
  }
  USART0->ROUTEPEN &= ~USART_ROUTEPEN_TXPEN;     /* GPIO DOUT = 1 takes over */
  return ok;
}

bool ic301_play(uint8_t a, uint8_t b)
{
  const uint8_t cmd[3] = { IC301_PLAY_CMD, a, b };
  return ic301_send(cmd, sizeof(cmd));
}

bool ic301_busy(void)
{
  return GPIO_PinInGet(IC301_BUSY_PORT, IC301_BUSY_PIN) == 0u;
}

static void wait_ms(uint32_t ms)
{
  wait_us(ms * 1000u);
}

uint32_t ic301_wait_idle_ms(uint32_t timeout_ms)
{
  uint32_t t = 0u;
  while (!ic301_busy()) {
    if (t++ >= IC301_START_TMO_MS) {
      return 0u;
    }
    wait_ms(1u);
  }
  uint32_t low_ms = 0u;
  while (ic301_busy() && low_ms < timeout_ms) {
    wait_ms(1u);
    low_ms++;
  }
  return low_ms;
}
