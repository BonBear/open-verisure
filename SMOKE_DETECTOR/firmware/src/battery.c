#include "battery.h"

#include "em_adc.h"
#include "em_cmu.h"

#define BATT_SAMPLES  8u
#define ADC_FULLSCALE_MV 5000u   /* adcRef5V: AVDD is scaled internally, 0..5 V maps to 0..4095 */

uint16_t battery_read_mv(void)
{
  ADC_Init_TypeDef init = ADC_INIT_DEFAULT;
  ADC_InitSingle_TypeDef single = ADC_INITSINGLE_DEFAULT;
  uint32_t sum = 0u;

  CMU_ClockEnable(cmuClock_ADC0, true);
  init.timebase = ADC_TimebaseCalc(0);
  init.prescale = ADC_PrescaleCalc(1000000, 0);   /* 1 MHz ADC clock */
  ADC_Init(ADC0, &init);

  single.reference = adcRef5V;
  single.posSel = adcPosSelAVDD;
  single.acqTime = adcAcqTime16;
  ADC_InitSingle(ADC0, &single);

  for (uint32_t i = 0; i < BATT_SAMPLES; i++) {
    ADC_Start(ADC0, adcStartSingle);
    while ((ADC0->STATUS & ADC_STATUS_SINGLEDV) == 0u) {
    }
    sum += ADC_DataSingleGet(ADC0);
  }

  ADC_Reset(ADC0);                                 /* back to the lowest-power state */
  CMU_ClockEnable(cmuClock_ADC0, false);
  return (uint16_t)((sum * ADC_FULLSCALE_MV) / (BATT_SAMPLES * 4095u));
}

uint8_t battery_pct(uint16_t mv)
{
  if (mv >= 3000u) {
    return 100u;
  }
  if (mv <= 2200u) {
    return 0u;
  }
  return (uint8_t)(((uint32_t)(mv - 2200u) * 100u) / 800u);
}
