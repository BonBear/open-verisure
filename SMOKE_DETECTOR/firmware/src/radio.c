#include "radio.h"

#include "em_device.h"
#include "rail.h"
#include "rail_config.h"

/* Raw sub-GHz PA level (0..248). Not calibrated in dBm yet; well below the maximum and
 * enough for the bench. TODO: switch to RAIL_SetTxPowerDbm with the PA curves (EU 868.7-869.2 MHz
 * allows 25 mW ERP). */
#ifndef RADIO_TX_POWER_RAW
#define RADIO_TX_POWER_RAW   60u
#endif
#define RADIO_CHANNEL        0u
#define RADIO_TX_TIMEOUT_MS  100u

static RAIL_Handle_t s_rail;

/* Bring-up diagnostics, read over SWD. */
volatile uint32_t g_rail_first_ch;   /* first channel returned by RAIL_ConfigChannels */
volatile uint32_t g_rail_tx_power;   /* raw PA level RAIL reports back */
/* RAIL needs a power of two >= 64, aligned to RAIL_FIFO_ALIGNMENT */
static uint8_t s_tx_fifo[64] __attribute__((aligned(RAIL_FIFO_ALIGNMENT)));
static volatile bool s_tx_done;
static volatile bool s_tx_ok;

static void rail_events(RAIL_Handle_t handle, RAIL_Events_t events)
{
  if (events & RAIL_EVENT_CAL_NEEDED) {
    (void)RAIL_Calibrate(handle, NULL, RAIL_CAL_ALL_PENDING);
  }
  if (events & RAIL_EVENT_TX_PACKET_SENT) {
    s_tx_ok = true;
    s_tx_done = true;
  }
  if (events & (RAIL_EVENT_TX_ABORTED | RAIL_EVENT_TX_BLOCKED | RAIL_EVENT_TX_UNDERFLOW)) {
    s_tx_ok = false;
    s_tx_done = true;
  }
}

static RAIL_Config_t s_rail_cfg = {
  .eventsCallback = &rail_events,
};

bool radio_init(void)
{
  s_rail = RAIL_Init(&s_rail_cfg, NULL);
  if (s_rail == NULL) {
    return false;
  }
  if (RAIL_ConfigCal(s_rail, RAIL_CAL_ALL) != RAIL_STATUS_NO_ERROR) {
    return false;
  }
  g_rail_first_ch = RAIL_ConfigChannels(s_rail, channelConfigs[0], NULL);

  RAIL_TxPowerConfig_t pa = {
    .mode = RAIL_TX_POWER_MODE_SUBGIG,
    .voltage = 3000,      /* mV, PA supply = battery */
    .rampTime = 10,       /* us */
  };
  if (RAIL_ConfigTxPower(s_rail, &pa) != RAIL_STATUS_NO_ERROR ||
      RAIL_SetTxPower(s_rail, RADIO_TX_POWER_RAW) != RAIL_STATUS_NO_ERROR) {
    return false;
  }
  g_rail_tx_power = RAIL_GetTxPower(s_rail);
  if (RAIL_ConfigEvents(s_rail, RAIL_EVENTS_ALL,
                        RAIL_EVENT_CAL_NEEDED | RAIL_EVENT_TX_PACKET_SENT |
                        RAIL_EVENT_TX_ABORTED | RAIL_EVENT_TX_BLOCKED |
                        RAIL_EVENT_TX_UNDERFLOW) != RAIL_STATUS_NO_ERROR) {
    return false;
  }
  if (RAIL_ConfigSleep(s_rail, RAIL_SLEEP_CONFIG_TIMERSYNC_DISABLED) != RAIL_STATUS_NO_ERROR) {
    return false;
  }
  return RAIL_SetTxFifo(s_rail, s_tx_fifo, 0, sizeof(s_tx_fifo)) == sizeof(s_tx_fifo);
}

radio_sleep_t radio_sleep(void)
{
  bool allowed = false;

  (void)RAIL_Idle(s_rail, RAIL_IDLE, true);
  if (RAIL_Sleep(0, &allowed) != RAIL_STATUS_NO_ERROR) {
    return RADIO_SLEEP_ERROR;
  }
  return allowed ? RADIO_SLEEP_DEEP : RADIO_SLEEP_SHALLOW;
}

void radio_wake(uint32_t slept_us)
{
  (void)RAIL_Wake(slept_us);
}

#ifdef RADIO_TEST_CW
/* Bench diagnostic: unmodulated carrier on channel ch (0 = 868.950 MHz, 1..33 = bench sweep
 * 868.750 + (ch - 1) * 12.5 kHz) until the next call or reset. */
bool radio_test_cw(uint16_t ch)
{
  (void)RAIL_Idle(s_rail, RAIL_IDLE_ABORT, true);
  return RAIL_StartTxStream(s_rail, ch, RAIL_STREAM_CARRIER_WAVE) == RAIL_STATUS_NO_ERROR;
}
#endif

uint32_t radio_time_us(void)
{
  return RAIL_GetTime();
}

bool radio_send(const uint8_t *data, uint16_t len)
{
  s_tx_done = false;
  s_tx_ok = false;
  if (RAIL_WriteTxFifo(s_rail, data, len, true) != len) {
    return false;
  }
  if (RAIL_StartTx(s_rail, RADIO_CHANNEL, RAIL_TX_OPTIONS_DEFAULT, NULL) != RAIL_STATUS_NO_ERROR) {
    return false;
  }
  const uint32_t start = RAIL_GetTime();          /* us */
  while (!s_tx_done) {
    if ((RAIL_GetTime() - start) > RADIO_TX_TIMEOUT_MS * 1000u) {
      (void)RAIL_Idle(s_rail, RAIL_IDLE_ABORT, true);
      return false;
    }
  }
  return s_tx_ok;
}
