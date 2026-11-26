#include "power.h"

#include "em_cmu.h"
#include "em_core.h"
#include "em_device.h"
#include "em_emu.h"
#include "em_rtcc.h"

#include "radio.h"

#define RTCC_HZ          1024u     /* rtccCntPresc_32 on 32.768 kHz */
#define WAKE_CH          1
#define LFXO_POLL_LIMIT  20000000u /* ~1 s of status reads at 38.4 MHz */

void RTCC_IRQHandler(void)
{
  RTCC_IntClear(RTCC_IF_CC1);
}

bool power_init(void)
{
  CMU_LFXOInit_TypeDef lfxo = CMU_LFXOINIT_DEFAULT;
  bool lfxo_ok = false;

  CMU_ClockEnable(cmuClock_HFLE, true);
  CMU_LFXOInit(&lfxo);
  CMU_OscillatorEnable(cmuOsc_LFXO, true, false);
  for (uint32_t i = 0; i < LFXO_POLL_LIMIT; i++) {
    if (CMU->STATUS & CMU_STATUS_LFXORDY) {
      lfxo_ok = true;
      break;
    }
  }
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
  bool allowed = radio_sleep();
  while (allowed && (RTCC_CounterGet() - start) < ticks) {
    EMU_EnterEM2(true);                                 /* restores HFXO as HFCLK on wake */
    RTCC_IntClear(RTCC_IF_CC1);
  }
  radio_wake((uint32_t)(((uint64_t)(RTCC_CounterGet() - start) * 1000000u) / RTCC_HZ));
  CORE_EXIT_CRITICAL();

  if (!allowed) {                                       /* RAIL busy: wait in EM0 instead */
    while ((RTCC_CounterGet() - start) < ticks) {
    }
  }
}
