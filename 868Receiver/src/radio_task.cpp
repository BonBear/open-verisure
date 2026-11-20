/**
 * @file radio_task.cpp
 * @brief CC1101 receive path in its own FreeRTOS task (design: see radio_task.h).
 *
 * The CC1101 code moved here from main.cpp (readStatusSafe, enterRxMode, initRadioRegisters, the
 * FIFO servicing of radioService, radioTransmit as radioDoTx). Behaviour is kept, except for: the
 * bounded status re-read, IOCFG2/IOCFG0 = 0x2E, the TX wait (task sleep instead of a busy loop)
 * and the offline/self-heal handling. The functions are static so that nothing outside this file
 * can touch the CC1101 once the task runs.
 */

#include "radio_task.h"
#include <SPI.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <string.h>
#include "config_manager.h"
#include "hardware_pins.h"

RadioStatus   g_radioStatus   = { false, 0, 0, 0x01, -100, 0 };
RadioDiag     g_radioDiag     = {};
RadioLoopDiag g_radioLoopDiag = {};

#ifdef RADIO_EVAL
volatile uint32_t g_radioEvalPollDelayMs = 0;
static volatile uint32_t s_evalStallMs = 0;
#endif

// ---- static FreeRTOS objects (no heap) ----
static StaticTask_t   s_taskTcb;
alignas(16) static StackType_t s_taskStack[RADIO_TASK_STACK / sizeof(StackType_t)];
static TaskHandle_t   s_task = nullptr;
static QueueHandle_t  s_rxQ = nullptr;   // created once in radioTaskStart(), PSRAM (never freed)
static StaticQueue_t  s_cmdQBuf;
static uint8_t        s_cmdQStore[RADIO_CMD_QUEUE_DEPTH * sizeof(RadioCmd)];
static QueueHandle_t  s_cmdQ = nullptr;
static StaticSemaphore_t s_doneBuf;
static SemaphoreHandle_t s_done = nullptr;

// ---- task-local state ----
static bool     s_online = false;
static RadioTestSpiResult s_testResult;       // filled by the task, then s_doneSeq is set
static volatile uint32_t s_doneSeq = 0;       // seq of the last finished REINIT/TESTSPI command
static uint32_t s_nextSeq = 0;                // loop() side: seq of the next request
static volatile bool s_resetReq = false;
static volatile bool s_stallSuspect = false;   // set by readStatusSafe, evaluated by the task loop
static uint8_t  s_pendingDrops = 0;
static uint32_t s_lastProbeMs = 0;
static uint32_t s_lastStatusMs = 0;

/** Feed the task WDT, only from the radio task (radioInit() runs in loopTask, which is not
 *  subscribed: esp_task_wdt_reset() would log "task not found" there). */
static inline void wdtFeed() {
    if (s_task && xTaskGetCurrentTaskHandle() == s_task) esp_task_wdt_reset();
}
static uint32_t s_lastHwmMs = 0;
static uint32_t s_lastServiceUs = 0;
static bool     s_gapValid = false;
static uint8_t  s_lastRxBytes = 0;

// frame assembler (task-local)
static uint8_t  s_frame[RX_FRAME_LEN];
static uint8_t  s_frameLen = 0;
static uint32_t s_frameStartUs = 0;
static int8_t   s_frameFreqEst = 0;     // FREQEST at the start of the frame
static uint32_t s_partialStartUs = 0;
static bool     s_partialSet = false;

// volatile read-modify-write helpers (compound operators on volatile are deprecated in C++20)
static inline void bump(volatile uint32_t& v) { v = v + 1; }
static inline void maxTo(volatile uint32_t& v, uint32_t x) { if (x > v) v = x; }

static void diagClear() {
    RadioDiag& d = g_radioDiag;
    d.frames = 0; d.fifoOverflows = 0; d.partialDrops = 0; d.queueDrops = 0; d.queueHighWater = 0;
    d.maxGapUs = 0; d.gapOver5ms = 0; d.gapOver10ms = 0; d.gapOver20ms = 0;
    d.txDone = 0; d.txLate = 0; d.txTimeouts = 0; d.txAbortedRx = 0; d.maxTxLatencyUs = 0;
    d.slowStatusReads = 0; d.statusUnstable = 0; d.rxRestarts = 0; d.rxReinits = 0;
}

static void assemblerReset() {
    s_frameLen = 0;
    s_partialSet = false;
}

/**
 * @brief Safely read a CC1101 status register with de-glitching / double-sampling.
 * @param addr CC1101 status register address.
 * @return Stabilized register value.
 * @note Required by TI CC1101 Errata note when reading status registers while SPI clock is running.
 *       Bounded to RADIO_STATUS_READ_MAX reads (statusUnstable counts a give-up, unless
 *       countUnstable is false: RSSI changes by nature). A read that takes longer than
 *       RADIO_SPI_STALL_US flags a suspected lost module (s_stallSuspect).
 */
static uint8_t readStatusSafe(uint8_t addr, bool countUnstable = true) {
    const uint32_t t0 = radioNowUs();
    uint8_t val1 = ELECHOUSE_cc1101.SpiReadStatus(addr);
    uint8_t val2 = ELECHOUSE_cc1101.SpiReadStatus(addr);
    uint8_t n = 2;
    while (val1 != val2 && n < RADIO_STATUS_READ_MAX) {
        val1 = val2;
        val2 = ELECHOUSE_cc1101.SpiReadStatus(addr);
        n++;
    }
    if (val1 != val2 && countUnstable) bump(g_radioDiag.statusUnstable);
    if ((uint32_t)(radioNowUs() - t0) > RADIO_SPI_STALL_US) {
        s_stallSuspect = true;
        bump(g_radioDiag.slowStatusReads);
    }
    return val2;
}

/**
 * @brief Reset RX FIFO and put CC1101 into continuous receive mode (SRX).
 */
static void enterRxMode() {
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);
    delayMicroseconds(100);
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SFRX);
    delayMicroseconds(100);
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SRX);
}

/**
 * @brief (Re)attach SCK/MISO/MOSI to the SPI peripheral; CS stays a GPIO driven by the driver.
 *        Needed after anything called pinMode() on them (core 3.x detaches the pin then).
 */
static void radioSpiAttach() {
    SPI.end();
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
    pinMode(PIN_CS, OUTPUT);
    digitalWrite(PIN_CS, HIGH);
}

/**
 * @brief Configure all CC1101 registers to match the Verisure 868.950 MHz radio profile.
 */
static void initRadioRegisters() {
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);

    // No GDO line is wired: keep both GDO pins high-Z (0x2E)
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_IOCFG2,   0x2E);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_IOCFG0,   0x2E);

    // Intermediate Frequency (IF)
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FSCTRL1,  0x06);
    // Frequency offset (FSCTRL0, f_xosc / 2^14 = 1.587 kHz per step, RX and TX), stored in NVS
    // (CLI "freqoffset <n>", default DEFAULT_FREQ_OFFSET = 0). A module whose crystal runs low
    // receives the sensors above the channel (FREQEST, logged as "Freq. error" per packet); the
    // first test module needed 38 (~+60 kHz, beyond the frequency loop's +-50 kHz FOCCFG
    // FOC_LIMIT BW/4, so weak packets failed their CRC, 2026-11-17). The logged error should
    // stay near 0.
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FSCTRL0,  (byte)ConfigManager::get().freqOffset);

    // RF Frequency: 868.950 MHz (with 26.000 MHz crystal on standard CC1101 modules)
    // Formula: FREQ = (868.950 * 2^16) / 26.0 = 2190289 = 0x216BD1
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FREQ2,    0x21);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FREQ1,    0x6B);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FREQ0,    0xD1);
    wdtFeed();   // a dead bus (WaitMiso up to 200 ms per access) must not trip the task WDT

    // Modem Configuration: 38.383 kBaud, 2-FSK, 4B preamble, 30/32 sync bits
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_MDMCFG4,  0x8A);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_MDMCFG3,  0x83);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_MDMCFG2,  0x03);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_MDMCFG1,  0x22);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_MDMCFG0,  0xF8);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_CHANNR,   0x00);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_DEVIATN,  0x35); // 21.4 kHz deviation
    wdtFeed();

    // Sync Word: 0xD3 0x91
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_SYNC1,    0xD3);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_SYNC0,    0x91);

    // Automatic Gain Control (AGC)
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_AGCCTRL2, 0x43);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_AGCCTRL1, 0x40);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_AGCCTRL0, 0x95);
    wdtFeed();

    // Front-End configuration
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FREND1,   0x56);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FREND0,   0x10);

    // TX power for the camera downlink (PATABLE[0], ~+10 dBm at 868 MHz)
    byte pa = 0xC0;
    ELECHOUSE_cc1101.SpiWriteBurstReg(CC1101_PATABLE, &pa, 1);

    // Packet Configuration: fixed length RX_PKT_LEN (58) + status bytes, CRC checked in
    // software (see RX_FRAME_LEN); radioDoTx switches to 24 bytes with CRC for the downlink
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_PKTLEN,   RX_PKT_LEN);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_PKTCTRL1, 0x04); // Append RSSI and LQI
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_PKTCTRL0, 0x00); // Fixed length mode, CRC off
    wdtFeed();

    // State machine: Autocalibrate IDLE -> RX, Stay in RX after packet
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_MCSM2,    0x07);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_MCSM1,    0x3C); // 0x3C: RXOFF_MODE = Stay in RX!
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_MCSM0,    0x18);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FOCCFG,   0x16);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_BSCFG,    0x6C);

    // Frequency Synthesizer Calibration
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FSCAL3,   0xE9);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FSCAL2,   0x2A);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FSCAL1,   0x00);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FSCAL0,   0x1F);
    wdtFeed();

    // Hardware Test Registers (SWRS033 Table 61)
    ELECHOUSE_cc1101.SpiWriteReg(0x2C,            0x81);
    ELECHOUSE_cc1101.SpiWriteReg(0x2D,            0x35);
    ELECHOUSE_cc1101.SpiWriteReg(0x2E,            0x09);

    enterRxMode();
}

// ---- queue helpers ----

static void pushItem(RadioFrame& f) {
    f.dropsBefore = s_pendingDrops;
    if (xQueueSend(s_rxQ, &f, 0) == pdTRUE) {
        s_pendingDrops = 0;
        maxTo(g_radioDiag.queueHighWater, (uint32_t)uxQueueMessagesWaiting(s_rxQ));
    } else {
        bump(g_radioDiag.queueDrops);
        if (s_pendingDrops < 255) s_pendingDrops++;
    }
}

static void postEvent(uint8_t kind, uint8_t arg0) {
    RadioFrame f;
    memset(&f, 0, sizeof(f));
    f.kind = kind;
    f.data[0] = arg0;
    f.startUs = f.doneUs = radioNowUs();
    pushItem(f);
}

/** FIFO overflow or stalled partial frame: report the bytes read so far and how many were lost. */
static void postLoss(uint8_t kind, uint8_t lost) {
    RadioFrame f;
    memset(&f, 0, sizeof(f));
    f.kind = kind;
    f.len = lost;
    memcpy(f.data, s_frame, s_frameLen);
    f.freqEst = s_frameFreqEst;
    f.startUs = s_frameStartUs;
    f.doneUs = radioNowUs();
    pushItem(f);
}

static void goOffline() {
    s_online = false;
    g_radioStatus.online = false;
    assemblerReset();
    s_gapValid = false;
    s_lastProbeMs = millis();
    postEvent(RADIO_ITEM_OFFLINE, 0);
}

static bool versionOk(uint8_t v) { return v == 0x04 || v == 0x14; }

// ---- RX service (was radioService in main.cpp, minus the frame handling) ----

static void radioServiceFifo() {
    const uint32_t nowUs = radioNowUs();
    uint8_t rxBytesStatus = readStatusSafe(CC1101_RXBYTES);
    s_lastRxBytes = rxBytesStatus;

    // Check for RX FIFO overflow (Bit 7 = 1)
    if (rxBytesStatus & 0x80) {
        bump(g_radioDiag.fifoOverflows);
        postLoss(RADIO_ITEM_OVERFLOW, (uint8_t)(s_frameLen + (rxBytesStatus & 0x7F)));
        enterRxMode();
        assemblerReset();
        return;
    }

    uint8_t bytesInFifo = rxBytesStatus & 0x7F;
    uint8_t need = RX_FRAME_LEN - s_frameLen;
    // A frame (60 bytes) nearly fills the 64-byte FIFO, so it is read in pieces while it
    // comes in; the last byte is left in the FIFO until the frame is complete (errata).
    uint8_t take = bytesInFifo >= need ? need : (bytesInFifo > 1 ? bytesInFifo - 1 : 0);
    if (take) {
        // FREQEST is read while the first bytes come in: the frame is 58 bytes, so after a
        // 24-byte packet the frequency loop tracks noise for ~7 ms and drifts off
        if (s_frameLen == 0) {
            s_frameFreqEst = (int8_t)readStatusSafe(CC1101_FREQEST);
            // back-dated: the bytes already in the FIFO arrived ~208 us each (38.4 kBd) before now
            s_frameStartUs = nowUs - (uint32_t)bytesInFifo * 208U;
        }
        ELECHOUSE_cc1101.SpiReadBurstReg(CC1101_RXFIFO, s_frame + s_frameLen, take);
        s_frameLen += take;
        s_partialStartUs = nowUs;
        s_partialSet = true;
    }

    if (s_frameLen == RX_FRAME_LEN) {
        RadioFrame f;
        memcpy(f.data, s_frame, RX_FRAME_LEN);
        f.kind = RADIO_ITEM_FRAME;
        f.len = RX_FRAME_LEN;
        f.freqEst = s_frameFreqEst;
        f.startUs = s_frameStartUs;
        f.doneUs = radioNowUs();
        bump(g_radioDiag.frames);
        pushItem(f);
        assemblerReset();
        // RXOFF_MODE = RX (MCSM1 0x3C): the radio is already receiving the next packet.
        // A flush here cut it off; with picture chunks back to back that lost a third of
        // them. Restart RX only if the radio left it (e.g. after a TX).
        if ((readStatusSafe(CC1101_MARCSTATE) & 0x1F) != 0x0D) enterRxMode();
    } else if (s_frameLen > 0 || bytesInFifo > 0) {
        // a frame that stops coming in: drop it and restart RX
        if (!s_partialSet) {
            s_partialStartUs = nowUs;
            s_partialSet = true;
        }
        if ((uint32_t)(nowUs - s_partialStartUs) > RADIO_PARTIAL_TIMEOUT_MS * 1000UL) {
            bump(g_radioDiag.partialDrops);
            postLoss(RADIO_ITEM_PARTIAL, (uint8_t)(s_frameLen + bytesInFifo));
            enterRxMode();
            assemblerReset();
        }
    } else {
        s_partialSet = false;
    }
}

/** RSSI + MARCSTATE sample for the heartbeat/web/MQTT, only while the radio is idle. */
static void radioSampleStatus() {
    int raw = readStatusSafe(CC1101_RSSI, false);   // RSSI changes by nature: not "unstable"
    uint8_t marc = readStatusSafe(CC1101_MARCSTATE) & 0x1F;
    g_radioStatus.rssiDbm = (int16_t)((raw >= 128) ? ((raw - 256) / 2) - 74 : (raw / 2) - 74);
    g_radioStatus.marcState = marc;
    g_radioStatus.sampledMs = millis();
    // self-heal (we are never in TX here): IDLE, RXFIFO_OVERFLOW or TXFIFO_UNDERFLOW
    if (marc == 0x01) {
        // IDLE can also mean the chip was reset (brown-out, glitch): registers are back at their
        // defaults. FREQ2 is 0x21 in our profile (reset value 0x1E); no VERSION = module gone.
        if (!versionOk(ELECHOUSE_cc1101.SpiReadStatus(CC1101_VERSION))) {
            goOffline();
        } else if (ELECHOUSE_cc1101.SpiReadReg(CC1101_FREQ2) != 0x21) {
            initRadioRegisters();
            bump(g_radioDiag.rxReinits);
        } else {
            enterRxMode();
            bump(g_radioDiag.rxRestarts);
        }
    } else if (marc == 0x11 || marc == 0x16) {
        enterRxMode();
        bump(g_radioDiag.rxRestarts);
    }
}

// ---- commands ----

/**
 * @brief Send one 24-byte packet and return to RX (MCSM1 TXOFF_MODE = IDLE).
 */
static void radioDoTx(RadioCmd& c) {
    uint32_t now = radioNowUs();
    if ((uint32_t)(now - c.frameStartUs) > RADIO_TX_MAX_AGE_MS * 1000UL) {
        bump(g_radioDiag.txLate);
        return;
    }
    if (s_frameLen > 0) {
        bump(g_radioDiag.txAbortedRx);
        assemblerReset();
    }
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);
    // the camera receives 24 bytes with hardware CRC (RX here uses 58 bytes, CRC off)
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_PKTLEN, VERISURE_PACKET_LEN);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_PKTCTRL0, 0x04);
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SFTX);
    ELECHOUSE_cc1101.SpiWriteBurstReg(CC1101_TXFIFO, c.pkt, VERISURE_PACKET_LEN);
    ELECHOUSE_cc1101.SpiStrobe(CC1101_STX);
    const uint32_t stxUs = radioNowUs();
    maxTo(g_radioDiag.maxTxLatencyUs, (uint32_t)(stxUs - c.frameStartUs));
    // 24 bytes + preamble/sync take ~7 ms on air: sleep, then poll once per tick
    vTaskDelay(pdMS_TO_TICKS(7));
    bool done = false;
    while (!done && (uint32_t)(radioNowUs() - stxUs) < RADIO_TX_TIMEOUT_MS * 1000UL) {
        if ((readStatusSafe(CC1101_MARCSTATE) & 0x1F) == 0x01) done = true;
        else vTaskDelay(1);
    }
    if (done) bump(g_radioDiag.txDone); else bump(g_radioDiag.txTimeouts);
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_PKTLEN, RX_PKT_LEN);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_PKTCTRL0, 0x00);
    enterRxMode();
    s_gapValid = false;
}

/** CLI testspi: the same sequence as before, run here because the task owns the SPI bus. */
static void radioDoTestSpi() {
    RadioTestSpiResult& r = s_testResult;
    pinMode(PIN_CS, OUTPUT);
    pinMode(PIN_MISO, INPUT_PULLUP);

    digitalWrite(PIN_CS, HIGH);
    delay(5);
    r.misoHigh = (uint8_t)digitalRead(PIN_MISO);

    digitalWrite(PIN_CS, LOW);
    delay(10);
    r.misoLow = (uint8_t)digitalRead(PIN_MISO);
    digitalWrite(PIN_CS, HIGH);

    radioSpiAttach();   // pinMode() above took MISO from the SPI peripheral (core 3.x)
    wdtFeed();

    // Manual CC1101 Power-On-Reset Sequence (TI Datasheet 19.1.2)
    digitalWrite(PIN_CS, HIGH);
    delayMicroseconds(50);
    digitalWrite(PIN_CS, LOW);
    delayMicroseconds(50);
    digitalWrite(PIN_CS, HIGH);
    delayMicroseconds(50);
    digitalWrite(PIN_CS, LOW);
    delay(5); // Wait for SO/MISO to go low

    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    r.sresSt = SPI.transfer(0x30); // CC1101_SRES
    digitalWrite(PIN_CS, HIGH);
    delay(10);
    wdtFeed();

    // Read PARTNUM (0x30 | 0xC0 = 0xF0) and VERSION (0x31 | 0xC0 = 0xF1)
    digitalWrite(PIN_CS, LOW);
    delayMicroseconds(50);
    r.partSt = SPI.transfer(0x30 | 0xC0);
    r.part = SPI.transfer(0x00);
    digitalWrite(PIN_CS, HIGH);
    delayMicroseconds(20);

    digitalWrite(PIN_CS, LOW);
    delayMicroseconds(50);
    r.verSt = SPI.transfer(0x31 | 0xC0);
    r.ver = SPI.transfer(0x00);
    digitalWrite(PIN_CS, HIGH);

    // Read MARCSTATE (0x35 | 0xC0 = 0xF5)
    digitalWrite(PIN_CS, LOW);
    delayMicroseconds(50);
    r.marcSt = SPI.transfer(0x35 | 0xC0);
    r.marc = SPI.transfer(0x00);
    digitalWrite(PIN_CS, HIGH);
    SPI.endTransaction();

    // SRES put every register back to its reset value (433 MHz, IDLE): restore the radio, but
    // only if the module answered (VERSION valid); otherwise it is gone
    wdtFeed();
    if (versionOk(r.ver)) {
        initRadioRegisters();
        assemblerReset();
        s_gapValid = false;
    } else if (s_online) {
        goOffline();
    }
}

static void radioDoCommand(RadioCmd& c) {
    switch (c.type) {
        case RADIO_CMD_TX:
            if (s_online) radioDoTx(c);
            break;
        case RADIO_CMD_REINIT:
            if (s_online) {
                if (versionOk(ELECHOUSE_cc1101.SpiReadStatus(CC1101_VERSION))) {
                    initRadioRegisters();
                    assemblerReset();
                    s_gapValid = false;
                } else {
                    goOffline();   // module gone: do not write registers into a dead bus
                }
            }
            s_doneSeq = c.seq;
            xSemaphoreGive(s_done);
            break;
        case RADIO_CMD_TESTSPI:
            radioDoTestSpi();
            s_doneSeq = c.seq;   // after s_testResult is complete
            xSemaphoreGive(s_done);
            break;
        default:
            break;
    }
}

// ---- the task ----

static void radioTaskMain(void*) {
    esp_task_wdt_add(nullptr);
    s_lastProbeMs = s_lastStatusMs = s_lastHwmMs = millis();
    for (;;) {
        esp_task_wdt_reset();
        if (s_resetReq) {
            s_resetReq = false;
            diagClear();
        }
#ifdef RADIO_EVAL
        if (s_evalStallMs) {
            uint32_t st = s_evalStallMs;
            s_evalStallMs = 0;
            vTaskDelay(pdMS_TO_TICKS(st));
        }
#endif
        const uint32_t nowMs = millis();
        // 1. Service the FIFO first, so that bytes already in it are read (and a complete frame is
        //    queued) before a TX command flushes the radio
        if (s_online) {
            const uint32_t nowUs = radioNowUs();
            if (s_gapValid) {
                const uint32_t gap = (uint32_t)(nowUs - s_lastServiceUs);
                maxTo(g_radioDiag.maxGapUs, gap);
                if (gap > 5000) bump(g_radioDiag.gapOver5ms);
                if (gap > 10000) bump(g_radioDiag.gapOver10ms);
                if (gap > 20000) bump(g_radioDiag.gapOver20ms);
            }
            s_lastServiceUs = nowUs;
            s_gapValid = true;

            radioServiceFifo();

            if (s_stallSuspect) {
                s_stallSuspect = false;   // counted in readStatusSafe (slowStatusReads)
                // a slow read alone (e.g. preempted) is not a lost module: check VERSION
                if (!versionOk(ELECHOUSE_cc1101.SpiReadStatus(CC1101_VERSION))) goOffline();
            } else if (s_online && s_frameLen == 0 && s_lastRxBytes == 0 &&
                       (uint32_t)(nowMs - s_lastStatusMs) >= RADIO_STATUS_INTERVAL_MS) {
                s_lastStatusMs = nowMs;
                radioSampleStatus();
            }
        }

        // 2. Commands (TX / REINIT / TESTSPI)
        RadioCmd c;
        while (xQueueReceive(s_cmdQ, &c, 0) == pdTRUE) radioDoCommand(c);

        // 3. Offline: probe for the module
        if (!s_online && (uint32_t)(millis() - s_lastProbeMs) >= RADIO_PROBE_INTERVAL_MS) {
            s_lastProbeMs = millis();   // fresh: goOffline() earlier in this pass set it after nowMs
            uint8_t ver = ELECHOUSE_cc1101.SpiReadStatus(CC1101_VERSION);
            if (versionOk(ver)) {   // init only with a valid VERSION (never write into a dead bus)
                initRadioRegisters();
                g_radioStatus.version = ver;
                g_radioStatus.partnum = ELECHOUSE_cc1101.SpiReadStatus(CC1101_PARTNUM);
                assemblerReset();
                s_gapValid = false;
                s_online = true;
                g_radioStatus.online = true;
                s_lastStatusMs = nowMs;
                postEvent(RADIO_ITEM_ONLINE, ver);
            }
        }

        if ((uint32_t)(nowMs - s_lastHwmMs) >= 1000) {
            s_lastHwmMs = nowMs;
            g_radioDiag.stackFreeMin = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
        }
        ulTaskNotifyTake(pdTRUE, RADIO_POLL_TICKS);
    }
}

// ---- public API ----

bool radioInit(uint8_t* version) {
    // setSpiPin() calls pinMode() on the pins, which on Arduino core 3.x detaches them from the
    // SPI peripheral, so SPI is attached afterwards (the library's own SPI.begin() in Init()
    // then does nothing).
    ELECHOUSE_cc1101.setSpiPin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
    radioSpiAttach();
    ELECHOUSE_cc1101.Init();
    delay(50);
    uint8_t ver = ELECHOUSE_cc1101.SpiReadStatus(CC1101_VERSION);
    if (version) *version = ver;
    g_radioStatus.version = ver;
    if (!versionOk(ver)) return false;
    g_radioStatus.partnum = ELECHOUSE_cc1101.SpiReadStatus(CC1101_PARTNUM);
    initRadioRegisters();
    return true;
}

bool radioTaskStart(bool online) {
    if (s_task) return true;
    // RX queue in PSRAM (static internal RAM did not fit .dram0.bss); task-to-task use only,
    // never touched from an ISR. Created once at boot, never deleted.
    QueueHandle_t rxQ = xQueueCreateWithCaps(RADIO_QUEUE_DEPTH, sizeof(RadioFrame), MALLOC_CAP_SPIRAM);
    if (!rxQ) return false;   // nothing published: no queues, no task, status stays offline
    QueueHandle_t cmdQ = xQueueCreateStatic(RADIO_CMD_QUEUE_DEPTH, sizeof(RadioCmd), s_cmdQStore, &s_cmdQBuf);
    SemaphoreHandle_t done = xSemaphoreCreateBinaryStatic(&s_doneBuf);
    if (!cmdQ || !done) return false;
    s_rxQ = rxQ;
    s_cmdQ = cmdQ;
    s_done = done;
    s_online = online;   // task-local, read by the task once it runs
    g_radioStatus.online = online;   // written before the task exists (single writer afterwards)
    TaskHandle_t t = xTaskCreateStatic(radioTaskMain, "radio", sizeof(s_taskStack) / sizeof(StackType_t),
                                       nullptr, RADIO_TASK_PRIO, s_taskStack, &s_taskTcb);
    if (!t) {
        s_online = false;
        g_radioStatus.online = false;
        s_rxQ = s_cmdQ = nullptr;
        s_done = nullptr;
        return false;
    }
    s_task = t;
    return true;
}

bool radioReceive(RadioFrame* out, uint32_t waitMs) {
    if (!s_rxQ) {
        if (waitMs) delay(waitMs);
        return false;
    }
    return xQueueReceive(s_rxQ, out, pdMS_TO_TICKS(waitMs)) == pdTRUE;
}

static bool sendCmd(const RadioCmd& c) {
    if (!s_task || !s_cmdQ || xQueueSend(s_cmdQ, &c, 0) != pdTRUE) return false;
    xTaskNotifyGive(s_task);
    return true;
}

bool radioRequestTx(const uint8_t pkt[VERISURE_PACKET_LEN], uint32_t frameStartUs) {
    RadioCmd c;
    c.type = RADIO_CMD_TX;
    memcpy(c.pkt, pkt, VERISURE_PACKET_LEN);
    c.frameStartUs = frameStartUs;
    c.seq = 0;
    return sendCmd(c);
}

/**
 * Send a command that signals completion through s_done; false if not sent or timed out. Every
 * request carries a sequence number and the task publishes the seq of the command it finished
 * (s_doneSeq), so a late completion of a timed-out request is recognised and ignored.
 */
static bool sendAndWait(uint8_t type, uint32_t waitMs) {
    if (!s_task || !s_cmdQ || !s_done) return false;
    xSemaphoreTake(s_done, 0);   // clear a stale give from an earlier timed-out request
    RadioCmd c;
    memset(&c, 0, sizeof(c));
    c.type = type;
    c.seq = ++s_nextSeq;
    if (!sendCmd(c)) return false;
    const uint32_t t0 = millis();
    for (;;) {
        const uint32_t elapsed = (uint32_t)(millis() - t0);
        const uint32_t remaining = elapsed < waitMs ? waitMs - elapsed : 0;
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(remaining)) != pdTRUE) return s_doneSeq == c.seq;
        if (s_doneSeq == c.seq) return true;   // else: a stale completion, keep waiting
    }
}

bool radioRequestReinit(uint32_t waitMs) {
    return sendAndWait(RADIO_CMD_REINIT, waitMs);
}

bool radioRequestTestSpi(RadioTestSpiResult* out, uint32_t waitMs) {
    if (!sendAndWait(RADIO_CMD_TESTSPI, waitMs)) return false;
    // s_testResult belongs to the finished command (only one request is outstanding; the task
    // writes it before publishing s_doneSeq and only starts the next command later)
    if (out) *out = s_testResult;
    return true;
}

void radioDiagReset() {
    memset(&g_radioLoopDiag, 0, sizeof(g_radioLoopDiag));
    s_resetReq = true;
    if (s_task) xTaskNotifyGive(s_task);
}

void radioDiagAppendJson(String& out) {
    const RadioDiag& d = g_radioDiag;
    const RadioLoopDiag& l = g_radioLoopDiag;
    out += "\"frames\":" + String((unsigned long)d.frames) + ",";
    out += "\"fifo_overflows\":" + String((unsigned long)d.fifoOverflows) + ",";
    out += "\"partial_drops\":" + String((unsigned long)d.partialDrops) + ",";
    out += "\"queue_drops\":" + String((unsigned long)d.queueDrops) + ",";
    out += "\"queue_high_water\":" + String((unsigned long)d.queueHighWater) + ",";
    out += "\"max_gap_us\":" + String((unsigned long)d.maxGapUs) + ",";
    out += "\"gap_gt_5ms\":" + String((unsigned long)d.gapOver5ms) + ",";
    out += "\"gap_gt_10ms\":" + String((unsigned long)d.gapOver10ms) + ",";
    out += "\"gap_gt_20ms\":" + String((unsigned long)d.gapOver20ms) + ",";
    out += "\"tx_done\":" + String((unsigned long)d.txDone) + ",";
    out += "\"tx_late\":" + String((unsigned long)d.txLate) + ",";
    out += "\"tx_timeouts\":" + String((unsigned long)d.txTimeouts) + ",";
    out += "\"tx_aborted_rx\":" + String((unsigned long)d.txAbortedRx) + ",";
    out += "\"max_tx_latency_us\":" + String((unsigned long)d.maxTxLatencyUs) + ",";
    out += "\"slow_status_reads\":" + String((unsigned long)d.slowStatusReads) + ",";
    out += "\"status_unstable\":" + String((unsigned long)d.statusUnstable) + ",";
    out += "\"rx_restarts\":" + String((unsigned long)d.rxRestarts) + ",";
    out += "\"rx_reinits\":" + String((unsigned long)d.rxReinits) + ",";
    out += "\"stack_free\":" + String((unsigned long)d.stackFreeMin) + ",";
    out += "\"poll_answers\":" + String((unsigned long)l.pollAnswers) + ",";
    out += "\"poll_late\":" + String((unsigned long)l.pollLate) + ",";
    out += "\"max_poll_answer_age_us\":" + String((unsigned long)l.maxPollAnswerAgeUs) + ",";
    out += "\"max_drain_gap_ms\":" + String((unsigned long)l.maxDrainGapMs);
}

#ifdef RADIO_EVAL
void radioEvalStall(uint32_t ms) {
    s_evalStallMs = ms > 3000 ? 3000 : ms;
    if (s_task) xTaskNotifyGive(s_task);
}
#endif
