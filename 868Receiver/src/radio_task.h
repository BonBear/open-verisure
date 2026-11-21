/**
 * @file radio_task.h
 * @brief CC1101 receive path in its own FreeRTOS task.
 *
 * WHY A TASK: the 64-byte CC1101 RX FIFO (no GDO line is wired, so no interrupt) overflows after
 * ~21 ms without a read (~13 ms on a channel full of back-to-back frames). loop() is blocked far
 * longer than that by the web server (handleClient 10-28 ms), flash writes, MQTT and USB prints.
 * A task that only polls the FIFO preempts loop() and keeps the FIFO read regardless.
 *
 * DESIGN
 *  - Ownership rule: after radioTaskStart() radio_task.cpp is the ONLY code that touches the
 *    CC1101 and the SPI bus (FIFO servicing, TX, register init, hot-plug probe, status sampling,
 *    the testspi sequence). Everybody else uses the request API below or the cached values in
 *    g_radioStatus / g_radioDiag. The task never prints, allocates, encrypts or touches NVS.
 *  - Priority RADIO_TASK_PRIO 5: above loopTask (1), below tcpip/event/esp_timer/Wi-Fi/USB tasks.
 *    It sleeps RADIO_POLL_TICKS (1 ms) between FIFO polls (ulTaskNotifyTake), so IDLE still runs.
 *  - Queue: complete 60-byte frames (and overflow/partial/online/offline events) are copied by
 *    value into ONE FIFO queue (RadioFrame, 256 items, created once at boot in PSRAM with
 *    xQueueCreateWithCaps because static internal RAM overflowed .dram0.bss; used task-to-task
 *    only, never from an ISR); loop() drains it in arrival order.
 *    Frame handling (CRC, crypto, replay, image receiver, MQTT, printing) is unchanged and stays in
 *    loop(). One queue only: the picture ACK (a poll answer without resend request) must come after
 *    all earlier frames were processed. A full queue never blocks the task: the item is dropped
 *    and counted (queueDrops, carried to the next item as dropsBefore).
 *  - Poll answer: built in loop() (answerCameraPoll) and sent by the task (radioRequestTx). The
 *    camera listens ~0.1 s after its poll, so there is a deadline guard on both sides: loop()
 *    skips the answer when the poll frame is older than POLL_ANSWER_MAX_AGE_MS (checked BEFORE the
 *    key derivation and downlinkCommandForPoll, so a pending resend/command is not consumed by an
 *    answer that is never sent), and the task drops a request older than RADIO_TX_MAX_AGE_MS at TX
 *    time (txLate). Timestamps are (uint32_t)esp_timer_get_time(); compare by unsigned difference.
 *  - Only loop() (CLI, web, MQTT callbacks) issues commands. No heap use at run time: task stack,
 *    command queue and semaphore are static (internal RAM); the RX queue is the single boot-time
 *    PSRAM allocation. If it cannot be created radioTaskStart() returns false (no task).
 *  - CC1101 errata handling is kept: status registers are read twice (SWRZ020, bounded here), the
 *    last FIFO byte of a frame is left in the FIFO until the frame is complete.
 *  - Flash writes (NVS, OTA) suspend all tasks: FIFO overflows are possible then (counted).
 *
 * Host unit tests (868Receiver/test_host) do not build this file; they need no FreeRTOS stubs.
 */

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <esp_timer.h>

/** @brief Length of sensor telemetry payload: 8B Header + 16B AES Ciphertext. */
#define VERISURE_PACKET_LEN 24

/** @brief MD CAM image packet (type 0x82): 8B header + 3 AES blocks (40 bytes of picture). */
#define BIG_PACKET_LEN      56

/**
 * @brief Receive frame. The radio always takes BIG_PACKET_LEN + 2 bytes (fixed length, hardware
 * CRC off) plus the RSSI/LQI status bytes; the CRC is checked in software: after 56 bytes a
 * camera image packet, after 24 bytes a sensor packet (followed by ~7 ms of noise, during which
 * another packet would be missed). So the sensors keep their 24-byte packets, and the camera
 * sends 5x the picture data per packet.
 */
#define RX_PKT_LEN          (BIG_PACKET_LEN + 2)
#define RX_FRAME_LEN        (RX_PKT_LEN + 2)

#define RADIO_TASK_PRIO          5     // above loopTask (1); below tcpip 18 / events 19,20 / esp_timer 22 / Wi-Fi 23 / usbd 24
#define RADIO_TASK_STACK         4096  // bytes (ESP-IDF), static
#define RADIO_POLL_TICKS         1     // 1 ms at FREERTOS_HZ 1000
#define RADIO_QUEUE_DEPTH        256   // 256 x 76 B = 19.5 KB PSRAM (covers MQTT reconnect stalls)
#define RADIO_CMD_QUEUE_DEPTH    4
#define RADIO_PARTIAL_TIMEOUT_MS 50    // a frame that stops coming in is dropped after this
#define RADIO_PROBE_INTERVAL_MS  2000  // offline VERSION probe
#define RADIO_STATUS_INTERVAL_MS 500   // RSSI/MARCSTATE sample, only between frames
#define RADIO_RECAL_INTERVAL_MS  300000UL // periodic RX restart (recalibration) when idle
#define RADIO_TX_TIMEOUT_MS      30
#define RADIO_TX_MAX_AGE_MS      105   // hard stop: poll frame start -> STX (to be tuned on the bench)
#define RADIO_STATUS_READ_MAX    4     // bound for readStatusSafe
#define RADIO_SPI_STALL_US       5000  // a status read this slow = module lost (verified by a VERSION read)
#define POLL_ANSWER_MAX_AGE_MS   95    // loop-side skip threshold (to be tuned on the bench)

#define RADIO_NUM_MODULES        2
#define RADIO_CH_MAIN            0
#define RADIO_CH_PCH             1
#define RADIO_FLAG_CAL           0x01
#define RADIO_FLAG_TX_OVERLAP    0x02

enum RadioItemKind : uint8_t { RADIO_ITEM_FRAME, RADIO_ITEM_OVERFLOW, RADIO_ITEM_PARTIAL,
                               RADIO_ITEM_ONLINE, RADIO_ITEM_OFFLINE };

struct RadioFrame {               // copied by value through the RX queue
    uint8_t  data[RX_FRAME_LEN];  // 60: frame + RSSI + LQI; OVERFLOW/PARTIAL: bytes read so far (ID in [0..7]); ONLINE: [0] = VERSION
    uint8_t  kind;
    uint8_t  len;                 // FRAME 60; OVERFLOW/PARTIAL: bytes lost (frameLen + RXBYTES)
    int8_t   freqEst;             // FREQEST when the first bytes were taken
    uint8_t  dropsBefore;         // items lost to a full queue just before this one (saturating)
    uint32_t startUs;             // (uint32_t)esp_timer_get_time() at first bytes; differences only
    uint32_t doneUs;              // frame complete / event time
    uint8_t  module;              // 0 = main (PIN_CS), 1 = picture channel (PIN_CS2)
    uint8_t  channel;             // RADIO_CH_MAIN or RADIO_CH_PCH
    uint8_t  flags;               // RADIO_FLAG_CAL | RADIO_FLAG_TX_OVERLAP
    uint8_t  reserved;
};
static_assert(sizeof(RadioFrame) == 76, "layout");

enum RadioCmdType : uint8_t { RADIO_CMD_TX = 1, RADIO_CMD_REINIT, RADIO_CMD_TESTSPI, RADIO_CMD_SETMODE };
struct RadioCmd {
    uint8_t  type;
    uint8_t  module;
    uint8_t  arg;
    uint8_t  pkt[VERISURE_PACKET_LEN];
    uint32_t frameStartUs;
    uint32_t seq;
};
struct RadioTestSpiResult { uint8_t misoHigh, misoLow, sresSt, partSt, part, verSt, ver, marcSt, marc; };

/** Cached radio state, written by the task only (single writer, aligned <= 32 bit). */
struct RadioStatus {
    volatile bool     online;
    volatile uint8_t  version;
    volatile uint8_t  partnum;
    volatile uint8_t  marcState;
    volatile int16_t  rssiDbm;
    volatile uint32_t sampledMs;   // millis() of the last RSSI/MARCSTATE sample
};

/**
 * Task-side counters since boot (single writer: the task).
 * txDone counts TX sequences the radio finished (MARCSTATE back in IDLE); compare it with the
 * loop-side pollAnswers (answers queued): pollAnswers - txDone - txLate - txTimeouts = answers
 * still pending or dropped because the radio was offline.
 * slowStatusReads = status reads that took more than RADIO_SPI_STALL_US (preemption, flash write
 * or a lost module; a lost module is confirmed by a VERSION read and then goes offline).
 * statusUnstable = a status register that still changed after RADIO_STATUS_READ_MAX reads (RSSI is
 * not counted, it changes by nature). rxRestarts = RX restarted by the MARCSTATE self-heal;
 * rxReinits = registers found reset (FREQ2 != 0x21) and rewritten.
 */
struct RadioDiag {
    volatile uint32_t frames, fifoOverflows, partialDrops, queueDrops, queueHighWater;
    volatile uint32_t maxGapUs, gapOver5ms, gapOver10ms, gapOver20ms;
    volatile uint32_t txDone, txLate, txTimeouts, txAbortedRx, maxTxLatencyUs;
    volatile uint32_t slowStatusReads, statusUnstable, rxRestarts, rxReinits;
    volatile uint32_t stackFreeMin;   // bytes, from uxTaskGetStackHighWaterMark
};

/** loop()-side counters (single writer: loop()). */
struct RadioLoopDiag {
    uint32_t pollAnswers, pollLate, maxPollAnswerAgeUs, maxDrainGapMs;
};

extern RadioStatus   g_radioStatus;
extern RadioStatus   g_radioStatus2;
extern RadioDiag     g_radioDiag;
extern RadioDiag     g_radioDiag2;
extern RadioLoopDiag g_radioLoopDiag;

/** @brief Microsecond timestamp (wraps after 71 min): compare only by unsigned difference. */
static inline uint32_t radioNowUs() { return (uint32_t)esp_timer_get_time(); }

/** setup(), before the task exists: SPI attach, probe, register init for both modules.
 *  Returns true if module 0 is online. */
bool radioInit(uint8_t* version, uint8_t* version2 = nullptr, bool* online2 = nullptr);
/** Create the queues and the task. online/online2 = result of radioInit(). False (nothing
 *  usable, status stays offline) if the PSRAM queue cannot be created. */
bool radioTaskStart(bool online, bool online2 = false);

/** loop() only: next queue item, waiting up to waitMs (0 = poll). */
bool radioReceive(RadioFrame* out, uint32_t waitMs);
/** Queue a 24-byte downlink packet (module 0 only); asynchronous. frameStartUs = startUs of the poll frame. */
bool radioRequestTx(const uint8_t pkt[VERISURE_PACKET_LEN], uint32_t frameStartUs);
/** Re-run the register init (new freqoffset) on module 0 or 1; waits up to waitMs for completion. */
bool radioRequestReinit(uint8_t module, uint32_t waitMs);
/** Run the CLI testspi sequence in the task on module 0 or 1; waits up to waitMs for the result. */
bool radioRequestTestSpi(uint8_t module, RadioTestSpiResult* out, uint32_t waitMs);
/** Toggle module 1 between PCH (869.525 MHz, calMode=false) and CAL (868.950 MHz, calMode=true). */
bool radioRequestSetMode(bool calMode, uint32_t waitMs);
/** Current module 1 mode (false = PCH, true = CAL). */
bool radioIsCalMode();
/** Zero all counters/maxima (the task consumes a flag; loop-side counters are cleared at once). */
void radioDiagReset();
/** Append the diagnostics as JSON members (no braces) for /api/status ("radio_diag" / "radio_diag2"). */
void radioDiagAppendJson(String& out, uint8_t module = 0);

#ifdef RADIO_EVAL
/** Evaluation only: the task stops servicing the FIFO once for ms (max 3000, below the task WDT). */
void radioEvalStall(uint32_t ms);
/** Evaluation only: delay in loop() before radioRequestTx (set by CLI polldelay). */
extern volatile uint32_t g_radioEvalPollDelayMs;
#endif
