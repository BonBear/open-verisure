/*
 * GW-SD3 bring-up test firmware (EFR32FG14P231F256GM48).
 *
 * Pulses the smoke-chamber LED (LED81, PF4) for 100 us every 500 ms and
 * increments a heartbeat counter that can be read over SWD. No radio, no
 * peripherals besides GPIO. The pulse is kept short on purpose: the stock
 * firmware drives LED81 with ~220 mA pulses, so never leave it on.
 */
#include <stdint.h>
#include "em_device.h"

#define LED81_PORT        5u      /* gpioPortF */
#define LED81_PIN         4u
#define LED_PULSE_US      100u
#define LED_PERIOD_MS     500u

/* Read over SWD to verify the firmware runs (see firmware/README.md). */
volatile uint32_t g_heartbeat;

static void dwt_init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0u;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void delay_us(uint32_t us)
{
  const uint32_t start = DWT->CYCCNT;
  const uint32_t ticks = (SystemCoreClock / 1000000u) * us;

  while ((DWT->CYCCNT - start) < ticks) {
  }
}

int main(void)
{
  dwt_init();

  CMU->HFBUSCLKEN0 |= CMU_HFBUSCLKEN0_GPIO;

  /* PF4 = push-pull output, driven low before the mode is enabled. */
  GPIO->P[LED81_PORT].DOUT &= ~(1u << LED81_PIN);
  GPIO->P[LED81_PORT].MODEL = (GPIO->P[LED81_PORT].MODEL & ~_GPIO_P_MODEL_MODE4_MASK)
                              | GPIO_P_MODEL_MODE4_PUSHPULL;

  for (;;) {
    GPIO->P[LED81_PORT].DOUT |= (1u << LED81_PIN);
    delay_us(LED_PULSE_US);
    GPIO->P[LED81_PORT].DOUT &= ~(1u << LED81_PIN);

    g_heartbeat++;
    delay_us(LED_PERIOD_MS * 1000u - LED_PULSE_US);
  }
}
