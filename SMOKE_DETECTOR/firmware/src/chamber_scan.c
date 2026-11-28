/*
 * Bench probe (build with -DCHAMBER_SCAN): find where the optical chamber AFE (IC81, C4660)
 * reaches the EFR32. For each enable configuration (none, PD13, PD15, PD13+PD15) it reads every
 * candidate pin with the ADC (VDD reference) statically and around a short PC6 pulse, plus AVDD
 * (5 V reference) to see the supply sag. Results stay in g_scan, read over SWD with pyOCD
 * (connect_mode=attach). Never part of a release build.
 */
#ifdef CHAMBER_SCAN

#include "chamber_scan.h"

#include "em_adc.h"
#include "em_cmu.h"
#include "em_core.h"
#include "em_device.h"
#include "em_gpio.h"

#include "radio.h"

#define N_CFG        4u
#define N_IN         25u        /* 24 pins + AVDD */
#define N_T          8u
#define REPEATS      4u
#define PULSE_US     120u
#define GAP_MS       50u
#define SETTLE_MS    50u
#define ADC_TO_US    100u
#define SCAN_MAGIC   0x5CA11ED5u

#define ST_ADC_TIMEOUT   0x1u
#define ST_APORT_CONFL   0x2u

typedef struct {
  uint8_t port;
  uint8_t pin;
  ADC_PosSel_TypeDef sel;
} scan_in_t;

/* APORT mapping: Port C -> CHn, F -> CH16+n, A -> CH8+n, D -> CHn-8, B -> CH16+n (1X/3X even,
 * 1Y/3Y odd). Port B entries are unverified. Last entry: AVDD with the 5 V reference. */
static const scan_in_t s_in[N_IN] = {
  {gpioPortF, 2, adcPosSelAPORT1XCH18}, {gpioPortF, 3, adcPosSelAPORT1YCH19},
  {gpioPortF, 4, adcPosSelAPORT1XCH20}, {gpioPortF, 5, adcPosSelAPORT1YCH21},
  {gpioPortF, 6, adcPosSelAPORT1XCH22}, {gpioPortF, 7, adcPosSelAPORT1YCH23},
  {gpioPortC, 6, adcPosSelAPORT1XCH6},  {gpioPortC, 7, adcPosSelAPORT1YCH7},
  {gpioPortC, 8, adcPosSelAPORT1XCH8},  {gpioPortC, 9, adcPosSelAPORT1YCH9},
  {gpioPortC, 10, adcPosSelAPORT1XCH10}, {gpioPortC, 11, adcPosSelAPORT1YCH11},
  {gpioPortA, 0, adcPosSelAPORT3XCH8},  {gpioPortA, 1, adcPosSelAPORT3YCH9},
  {gpioPortA, 2, adcPosSelAPORT3XCH10}, {gpioPortA, 3, adcPosSelAPORT3YCH11},
  {gpioPortA, 4, adcPosSelAPORT3XCH12}, {gpioPortA, 5, adcPosSelAPORT3YCH13},
  {gpioPortD, 13, adcPosSelAPORT3YCH5}, {gpioPortD, 14, adcPosSelAPORT3XCH6},
  {gpioPortD, 15, adcPosSelAPORT3YCH7}, {gpioPortB, 11, adcPosSelAPORT3YCH27},
  {gpioPortB, 12, adcPosSelAPORT3XCH28}, {gpioPortB, 13, adcPosSelAPORT3YCH29},
  {0, 0, adcPosSelAVDD},
};

/* Sample times relative to the PC6 rising edge (us); PC6 falls at PULSE_US. */
static const int32_t s_t[N_T] = {-5, 10, 40, 80, 115, 130, 170, 320};

volatile struct {
  uint32_t magic;
  uint32_t seq;
  uint32_t status;
  uint32_t din[N_CFG][2][5];       /* [cfg][0 static, 1 at ~+120 us, around PC6 release][port A,B,C,D,F] */
  uint16_t stat[N_CFG][N_IN];      /* static, sum of 4 */
  uint16_t pulse[N_CFG][N_IN][N_T];/* sum of REPEATS */
} g_scan;

static const GPIO_Port_TypeDef s_ports[5] = {gpioPortA, gpioPortB, gpioPortC, gpioPortD, gpioPortF};

static void wait_until(uint32_t t0, int32_t t_us)
{
  while ((int32_t)(radio_time_us() - t0) < t_us) {
  }
}

static void delay_ms(uint32_t ms)
{
  const uint32_t t0 = radio_time_us();
  while ((radio_time_us() - t0) < ms * 1000u) {
  }
}

static uint16_t adc_one(void)
{
  ADC_Start(ADC0, adcStartSingle);
  const uint32_t t0 = radio_time_us();
  while ((ADC0->STATUS & ADC_STATUS_SINGLEDV) == 0u) {
    if ((radio_time_us() - t0) >= ADC_TO_US) {
      g_scan.status |= ST_ADC_TIMEOUT;
      return 0u;
    }
  }
  if (ADC0->APORTCONFLICT != 0u) {
    g_scan.status |= ST_APORT_CONFL;
  }
  return (uint16_t)ADC_DataSingleGet(ADC0);
}

static void adc_select(uint32_t i, ADC_AcqTime_TypeDef acq)
{
  ADC_InitSingle_TypeDef single = ADC_INITSINGLE_DEFAULT;
  single.posSel = s_in[i].sel;
  single.reference = (s_in[i].sel == adcPosSelAVDD) ? adcRef5V : adcRefVDD;
  single.acqTime = acq;
  ADC_InitSingle(ADC0, &single);
  (void)adc_one();                                  /* discard: input/reference switch */
  (void)adc_one();
}

static void set_cfg(uint32_t cfg)
{
  GPIO_PinModeSet(gpioPortD, 13, (cfg & 1u) ? gpioModePushPull : gpioModeInputPull, (cfg & 1u) ? 1 : 0);
  GPIO_PinModeSet(gpioPortD, 15, (cfg & 2u) ? gpioModePushPull : gpioModeInputPull, (cfg & 2u) ? 1 : 0);
}

static void read_din(volatile uint32_t *dst)
{
  for (uint32_t p = 0; p < 5u; p++) {
    dst[p] = GPIO->P[s_ports[p]].DIN;
  }
}

static void pulse_one(uint32_t cfg, uint32_t i, bool log_din)
{
  uint16_t v[N_T];
  uint32_t din[5];
  CORE_DECLARE_IRQ_STATE;

  CORE_ENTER_CRITICAL();
  const uint32_t t0 = radio_time_us() + 10u;
  wait_until(t0, s_t[0]);
  v[0] = adc_one();
  wait_until(t0, 0);
  GPIO_PinOutSet(gpioPortC, 6);
  for (uint32_t k = 1; k < N_T; k++) {
    if (s_t[k] > (int32_t)PULSE_US && s_t[k - 1] <= (int32_t)PULSE_US) {
      if (log_din) {
        read_din(din);
      }
      wait_until(t0, PULSE_US);
      GPIO_PinOutClear(gpioPortC, 6);
    }
    wait_until(t0, s_t[k]);
    v[k] = adc_one();
    if (s_t[k] <= (int32_t)PULSE_US && (int32_t)(radio_time_us() - t0) >= (int32_t)PULSE_US) {
      GPIO_PinOutClear(gpioPortC, 6);              /* hard limit if a conversion ran late */
    }
  }
  GPIO_PinOutClear(gpioPortC, 6);
  CORE_EXIT_CRITICAL();

  for (uint32_t k = 0; k < N_T; k++) {
    g_scan.pulse[cfg][i][k] = (uint16_t)(g_scan.pulse[cfg][i][k] + v[k]);
  }
  if (log_din) {
    for (uint32_t p = 0; p < 5u; p++) {
      g_scan.din[cfg][1][p] = din[p];
    }
  }
}

void chamber_scan_run(void)
{
  ADC_Init_TypeDef init = ADC_INIT_DEFAULT;

  GPIO_PinModeSet(gpioPortC, 6, gpioModePushPull, 0);
  /* Sampled pins: plain input, no pull (PC6 stays an output; PD13/PD15/PF2 keep their config). */
  for (uint32_t i = 0; i + 1u < N_IN; i++) {
    const uint8_t port = s_in[i].port;
    const uint8_t pin = s_in[i].pin;
    if ((port == gpioPortC && pin == 6u) || (port == gpioPortD && (pin == 13u || pin == 15u)) ||
        (port == gpioPortF && pin == 2u)) {
      continue;
    }
    GPIO_PinModeSet((GPIO_Port_TypeDef)port, pin, gpioModeInput, 0);
  }

  CMU_ClockEnable(cmuClock_ADC0, true);
  init.timebase = ADC_TimebaseCalc(0);
  init.prescale = ADC_PrescaleCalc(9600000, 0);
  init.warmUpMode = adcWarmupKeepADCWarm;
  ADC_Init(ADC0, &init);

  for (;;) {
    g_scan.magic = 0u;
    for (uint32_t cfg = 0; cfg < N_CFG; cfg++) {
      for (uint32_t i = 0; i < N_IN; i++) {
        g_scan.stat[cfg][i] = 0u;
        for (uint32_t k = 0; k < N_T; k++) {
          g_scan.pulse[cfg][i][k] = 0u;
        }
      }
    }
    g_scan.status = 0u;

    for (uint32_t cfg = 0; cfg < N_CFG; cfg++) {
      set_cfg(cfg);
      delay_ms(SETTLE_MS);
      read_din(g_scan.din[cfg][0]);
      for (uint32_t i = 0; i < N_IN; i++) {
        adc_select(i, adcAcqTime256);
        for (uint32_t r = 0; r < REPEATS; r++) {
          g_scan.stat[cfg][i] = (uint16_t)(g_scan.stat[cfg][i] + adc_one());
        }
        adc_select(i, adcAcqTime32);
        for (uint32_t r = 0; r < REPEATS; r++) {
          pulse_one(cfg, i, (i == 0u && r == 0u));
          delay_ms(GAP_MS);
        }
      }
    }
    set_cfg(0u);                                     /* back to pins_init levels */
    GPIO_PinOutClear(gpioPortC, 6);
    g_scan.seq++;
    g_scan.magic = SCAN_MAGIC;
    delay_ms(10000u);
  }
}

#endif /* CHAMBER_SCAN */
