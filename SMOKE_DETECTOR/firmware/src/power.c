#include "power.h"

#include "em_cmu.h"
#include "em_core.h"
#include "em_device.h"
#include "em_emu.h"
#include "em_rtcc.h"

#include "radio.h"

#define RTCC_HZ          1024u     /* rtccCntPresc_32 on 32.768 kHz */
#define WAKE_CH          1
#define LFXO_TIMEOUT_US  2000000u  /* LFXORDY needs ~1.1-1.5 s (32K-cycle startup timeout) */

/* Sleeps RAIL could not allow in EM2 (spent in EM1 instead); read over SWD. */
volatile uint32_t g_sleep_refused;

void RTCC_IRQHandler(void)
{
  RTCC_IntClear(RTCC_IF_CC1);
}

bool power_init(void)
{
  CMU_LFXOInit_TypeDef lfxo = CMU_LFXOINIT_DEFAULT;

  CMU_ClockEnable(cmuClock_HFLE, true);
  CMU_LFXOInit(&lfxo);
  CMU_OscillatorEnable(cmuOsc_LFXO, true, false);
  const uint32_t t0 = radio_time_us();
  while ((CMU->STATUS & CMU_STATUS_LFXORDY) == 0u && (radio_time_us() - t0) < LFXO_TIMEOUT_US) {
  }
  const bool lfxo_ok = (CMU->STATUS & CMU_STATUS_LFXORDY) != 0u;
  if (lfxo_ok) {
    CMU_ClockSelectSet(cmuClock_LFE, cmuSelect_LFXO);
  } else {
    CMU_OscillatorEnable(cmuOsc_LFXO, false, false);
    CMU_OscillatorEnable(cmuOsc_LFRCO, true, true);
    CMU_ClockSelectSet(cmuClock_LFE, cmuSelect_LFRCO);
  }
  CMU_ClockEnable(cmuClock_RTCC, true);

  RTCC_Init_TypeDef init = RTCC_INIT_DEFAULT;          /* 1024 Hz tick, starts counting */
  RTCC_Init(&init);
  RTCC_CCChConf_TypeDef ch = RTCC_CH_INIT_COMPARE_DEFAULT;
  RTCC_ChannelInit(WAKE_CH, &ch);
  RTCC_IntClear(RTCC_IF_CC1);
  RTCC_IntEnable(RTCC_IF_CC1);
  NVIC_ClearPendingIRQ(RTCC_IRQn);
  NVIC_EnableIRQ(RTCC_IRQn);

  EMU_EM23Init_TypeDef em23 = EMU_EM23INIT_DEFAULT;
  EMU_EM23Init(&em23);
  return lfxo_ok;
}

void power_sleep_ms(uint32_t ms)
{
  const uint32_t ticks = (ms * RTCC_HZ) / 1000u;
  const uint32_t start = RTCC_CounterGet();
  const uint32_t wake = start + ticks;
  CORE_DECLARE_IRQ_STATE;

  RTCC_IntClear(RTCC_IF_CC1);
  RTCC_ChannelCCVSet(WAKE_CH, wake);

  /* PRIMASK critical section: the RTCC interrupt still ends WFI, its handler runs on exit. */
  CORE_ENTER_CRITICAL();
  const radio_sleep_t rs = radio_sleep();
  if (rs != RADIO_SLEEP_DEEP) {
    g_sleep_refused++;
  }
  while ((RTCC_CounterGet() - start) < ticks) {
    if (rs == RADIO_SLEEP_DEEP) {
      EMU_EnterEM2(true);                               /* restores HFXO as HFCLK on wake */
    } else {
      EMU_EnterEM1();                                   /* HF clocks keep running for RAIL */
    }
    RTCC_IntClear(RTCC_IF_CC1);
  }
  if (rs != RADIO_SLEEP_ERROR) {
    radio_wake((uint32_t)(((uint64_t)(RTCC_CounterGet() - start) * 1000000u) / RTCC_HZ));
  }
  CORE_EXIT_CRITICAL();
}
