/**
 * @file radio_task.cpp
 * @brief Dual-CC1101 receive/TX path in its own FreeRTOS task (design: see radio_task.h).
 *
 * Uses a minimal static CC1101 SPI driver (4 MHz MODE0, bounded MISO-low wait via gpio_get_level,
 * explicit IOCFG1 = 0x2E tri-state on both modules) instead of SmartRC-CC1101, which blocked
 * for 200 ms on a missing module and detached SPI pinmux on Arduino core 3.x.
 */

#include "radio_task.h"
#include <SPI.h>
#include <driver/gpio.h>
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

// ---- CC1101 SPI constants (SWRS061I) ----
#define CC1101_IOCFG2    0x00
#define CC1101_IOCFG1    0x01
#define CC1101_IOCFG0    0x02
#define CC1101_SYNC1     0x04
#define CC1101_SYNC0     0x05
#define CC1101_PKTLEN    0x06
#define CC1101_PKTCTRL1  0x07
#define CC1101_PKTCTRL0  0x08
#define CC1101_CHANNR    0x0A
#define CC1101_FSCTRL1   0x0B
#define CC1101_FSCTRL0   0x0C
#define CC1101_FREQ2     0x0D
#define CC1101_FREQ1     0x0E
#define CC1101_FREQ0     0x0F
#define CC1101_MDMCFG4   0x10
#define CC1101_MDMCFG3   0x11
#define CC1101_MDMCFG2   0x12
#define CC1101_MDMCFG1   0x13
#define CC1101_MDMCFG0   0x14
#define CC1101_DEVIATN   0x15
#define CC1101_MCSM2     0x16
#define CC1101_MCSM1     0x17
#define CC1101_MCSM0     0x18
#define CC1101_FOCCFG    0x19
#define CC1101_BSCFG     0x1A
#define CC1101_AGCCTRL2  0x1B
#define CC1101_AGCCTRL1  0x1C
#define CC1101_AGCCTRL0  0x1D
#define CC1101_FREND1    0x21
#define CC1101_FREND0    0x22
#define CC1101_FSCAL3    0x23
#define CC1101_FSCAL2    0x24
#define CC1101_FSCAL1    0x25
#define CC1101_FSCAL0    0x26
#define CC1101_TEST2     0x2C
#define CC1101_TEST1     0x2D
#define CC1101_TEST0     0x2E

#define CC1101_PATABLE   0x3E
#define CC1101_TXFIFO    0x3F
#define CC1101_RXFIFO    0x3F

#define CC1101_SRES      0x30
#define CC1101_SRX       0x34
#define CC1101_STX       0x35
#define CC1101_SIDLE     0x36
#define CC1101_SFRX      0x3A
#define CC1101_SFTX      0x3B

#define CC1101_PARTNUM   0x30
#define CC1101_VERSION   0x31
#define CC1101_FREQEST   0x32
#define CC1101_RSSI      0x34
#define CC1101_MARCSTATE 0x35
#define CC1101_RXBYTES   0x3B

RadioStatus   g_radioStatus   = { false, 0, 0, 0x01, -100, 0, 0 };
RadioStatus   g_radioStatus2  = { false, 0, 0, 0x01, -100, 0, 0 };
RadioDiag     g_radioDiag     = {};
RadioDiag     g_radioDiag2    = {};
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

// ---- per-module state ----
struct RadioModule {
    uint8_t      index;
    uint8_t      csPin;
    bool         canTx;
    bool         online;
    bool         calMode;          // module 1 only: false = PCH 869.525 MHz, true = CAL 868.950 MHz
    bool         stallSuspect;
    bool         gapValid;
    uint32_t     lastProbeMs;
    uint32_t     lastStatusMs;
    uint32_t     lastRecalMs;
    uint32_t     lastServiceUs;
    uint8_t      lastRxBytes;
    uint8_t      frame[RX_FRAME_LEN];
    uint8_t      frameLen;
    uint32_t     frameStartUs;
    int8_t       frameFreqEst;
    uint32_t     partialStartUs;
    bool         partialSet;
    RadioStatus* status;
    RadioDiag*   diag;
};

static RadioModule s_mod[RADIO_NUM_MODULES];

// ---- task-local shared state ----
static RadioTestSpiResult s_testResult;       // filled by the task, then s_doneSeq is set
static volatile uint32_t s_doneSeq = 0;       // seq of the last finished command
static uint32_t s_nextSeq = 0;                // loop() side: seq of the next request
static volatile bool s_resetReq = false;
static volatile bool s_calMode = false;       // mirrored from s_mod[1].calMode for radioIsCalMode()
static uint8_t  s_pendingDrops = 0;
static uint32_t s_lastHwmMs = 0;

// Module 0 TX window tracking for RADIO_FLAG_TX_OVERLAP on module 1
static bool     s_txActive = false;
static bool     s_hasLastTx = false;
static uint32_t s_txBeginUs = 0;
static uint32_t s_lastTxEndUs = 0;

/** Feed the task WDT, only from the radio task (radioInit() runs in loopTask before enableLoopWDT()). */
static inline void wdtFeed() {
    if (s_task && xTaskGetCurrentTaskHandle() == s_task) esp_task_wdt_reset();
}

// volatile read-modify-write helpers (compound operators on volatile are deprecated in C++20)
static inline void bump(volatile uint32_t& v) { v = v + 1; }
static inline void maxTo(volatile uint32_t& v, uint32_t x) { if (x > v) v = x; }

static void initModState() {
    memset(s_mod, 0, sizeof(s_mod));
    s_mod[0].index  = 0;
    s_mod[0].csPin  = PIN_CS;
    s_mod[0].canTx  = true;
    s_mod[0].status = &g_radioStatus;
    s_mod[0].diag   = &g_radioDiag;

    s_mod[1].index  = 1;
    s_mod[1].csPin  = PIN_CS2;
    s_mod[1].canTx  = false;
    s_mod[1].status = &g_radioStatus2;
    s_mod[1].diag   = &g_radioDiag2;
}

static void diagClearOne(RadioDiag& d) {
    d.frames = 0; d.fifoOverflows = 0; d.partialDrops = 0; d.queueDrops = 0; d.queueHighWater = 0;
    d.maxGapUs = 0; d.gapOver5ms = 0; d.gapOver10ms = 0; d.gapOver20ms = 0;
    d.txDone = 0; d.txLate = 0; d.txTimeouts = 0; d.txAbortedRx = 0; d.maxTxLatencyUs = 0;
    d.slowStatusReads = 0; d.statusUnstable = 0; d.rxRestarts = 0; d.rxReinits = 0;
}

static void diagClear() {
    diagClearOne(g_radioDiag);
    diagClearOne(g_radioDiag2);
}

static void assemblerReset(RadioModule& m) {
    m.frameLen = 0;
    m.partialSet = false;
}

static uint8_t moduleChannel(const RadioModule& m) {
    return (m.index == 0 || m.calMode) ? RADIO_CH_MAIN : RADIO_CH_PCH;
}

static uint8_t moduleFlags(const RadioModule& m) {
    return (m.index == 1 && m.calMode) ? RADIO_FLAG_CAL : 0;
}

static uint8_t expectedFreq1(const RadioModule& m) {
    return (m.index == 0 || m.calMode) ? 0x6B : 0x71;
}

// ---- minimal static CC1101 SPI driver ----

/**
 * @brief (Re)attach SCK/MISO/MOSI to the SPI peripheral; both CS pins stay HIGH outputs
 *        before SPI.begin() so neither CC1101 is ever selected simultaneously.
 */
static void radioSpiAttach() {
    pinMode(PIN_CS, OUTPUT);
    digitalWrite(PIN_CS, HIGH);
    pinMode(PIN_CS2, OUTPUT);
    digitalWrite(PIN_CS2, HIGH);
    SPI.end();
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
}

/**
 * @brief Begin an SPI transaction for csPin and wait up to waitUs for SO (MISO) to go LOW
 *        (CC1101 crystal ready). Uses gpio_get_level so Arduino core 3.x SPI pinmux stays intact.
 */
static bool ccBegin(uint8_t csPin, uint32_t waitUs = 200) {
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(csPin, LOW);
    const uint32_t t0 = radioNowUs();
    while (gpio_get_level((gpio_num_t)PIN_MISO) != 0) {
        if ((uint32_t)(radioNowUs() - t0) >= waitUs) {
            digitalWrite(csPin, HIGH);
            SPI.endTransaction();
            return false;
        }
    }
    return true;
}

static void ccEnd(uint8_t csPin) {
    digitalWrite(csPin, HIGH);
    SPI.endTransaction();
}

static uint8_t ccStrobe(uint8_t csPin, uint8_t strobe, bool* ok = nullptr) {
    if (!ccBegin(csPin)) {
        if (ok) *ok = false;
        return 0;
    }
    uint8_t st = SPI.transfer(strobe);
    ccEnd(csPin);
    if (ok) *ok = true;
    return st;
}

static bool ccWriteReg(uint8_t csPin, uint8_t addr, uint8_t val) {
    if (!ccBegin(csPin)) return false;
    SPI.transfer(addr);
    SPI.transfer(val);
    ccEnd(csPin);
    return true;
}

static bool ccWriteBurst(uint8_t csPin, uint8_t addr, const uint8_t* buf, uint8_t len) {
    if (!ccBegin(csPin)) return false;
    SPI.transfer(addr | 0x40);
    for (uint8_t i = 0; i < len; i++) SPI.transfer(buf[i]);
    ccEnd(csPin);
    return true;
}

static uint8_t ccReadReg(uint8_t csPin, uint8_t addr, bool* ok = nullptr) {
    if (!ccBegin(csPin)) {
        if (ok) *ok = false;
        return 0;
    }
    SPI.transfer(addr | 0x80);
    uint8_t val = SPI.transfer(0x00);
    ccEnd(csPin);
    if (ok) *ok = true;
    return val;
}

static uint8_t ccReadStatus(uint8_t csPin, uint8_t addr, bool* ok = nullptr) {
    if (!ccBegin(csPin)) {
        if (ok) *ok = false;
        return 0;
    }
    SPI.transfer(addr | 0xC0);
    uint8_t val = SPI.transfer(0x00);
    ccEnd(csPin);
    if (ok) *ok = true;
    return val;
}

static bool ccReadBurst(uint8_t csPin, uint8_t addr, uint8_t* buf, uint8_t len) {
    if (!ccBegin(csPin)) return false;
    SPI.transfer(addr | 0xC0);
    for (uint8_t i = 0; i < len; i++) buf[i] = SPI.transfer(0x00);
    ccEnd(csPin);
    return true;
}

static bool ccReset(uint8_t csPin) {
    digitalWrite(csPin, HIGH);
    delayMicroseconds(50);
    digitalWrite(csPin, LOW);
    delayMicroseconds(50);
    digitalWrite(csPin, HIGH);
    delayMicroseconds(50);
    if (!ccBegin(csPin, 2000)) return false;
    SPI.transfer(CC1101_SRES);
    const uint32_t t0 = radioNowUs();
    bool ready = false;
    while ((uint32_t)(radioNowUs() - t0) < 2000) {
        if (gpio_get_level((gpio_num_t)PIN_MISO) == 0) {
            ready = true;
            break;
        }
    }
    ccEnd(csPin);
    return ready;
}

// ---- CC1101 radio helpers ----

/**
 * @brief Safely read a CC1101 status register with de-glitching / double-sampling (SWRZ020).
 */
static uint8_t readStatusSafe(RadioModule& m, uint8_t addr, bool countUnstable = true) {
    const uint32_t t0 = radioNowUs();
    bool ok1 = false, ok2 = false;
    uint8_t val1 = ccReadStatus(m.csPin, addr, &ok1);
    uint8_t val2 = ccReadStatus(m.csPin, addr, &ok2);
    if (!ok1 || !ok2) {
        m.stallSuspect = true;
        if ((uint32_t)(radioNowUs() - t0) > RADIO_SPI_STALL_US) bump(m.diag->slowStatusReads);
        return 0;
    }
    uint8_t n = 2;
    while (val1 != val2 && n < RADIO_STATUS_READ_MAX) {
        val1 = val2;
        bool okn = false;
        val2 = ccReadStatus(m.csPin, addr, &okn);
        if (!okn) {
            m.stallSuspect = true;
            return 0;
        }
        n++;
    }
    if (val1 != val2 && countUnstable) bump(m.diag->statusUnstable);
    if ((uint32_t)(radioNowUs() - t0) > RADIO_SPI_STALL_US) {
        m.stallSuspect = true;
        bump(m.diag->slowStatusReads);
    }
    return val2;
}

static void enterIdle(uint8_t csPin) {
    ccStrobe(csPin, CC1101_SIDLE);
    const uint32_t t0 = radioNowUs();
    while ((uint32_t)(radioNowUs() - t0) < 1000) {
        bool ok = false;
        uint8_t marc = ccReadStatus(csPin, CC1101_MARCSTATE, &ok) & 0x1F;
        if (!ok || marc == 0x01) break;
        delayMicroseconds(50);
    }
}

/**
 * @brief Reset RX FIFO and put CC1101 into continuous receive mode (SRX).
 */
static void enterRxMode(RadioModule& m) {
    enterIdle(m.csPin);
    ccStrobe(m.csPin, CC1101_SFRX);
    delayMicroseconds(100);
    ccStrobe(m.csPin, CC1101_SRX);
    m.lastRecalMs = millis();
}

/** Write FREQ2/1/0 for module's current channel; caller must ensure IDLE (or calls enterIdle). */
static void writeModuleFreq(const RadioModule& m) {
    enterIdle(m.csPin);
    if (m.index == 0 || m.calMode) {
        // 868.950 MHz with 26 MHz crystal: (868.950 * 2^16) / 26.0 = 2190289 = 0x216BD1
        ccWriteReg(m.csPin, CC1101_FREQ2, 0x21);
        ccWriteReg(m.csPin, CC1101_FREQ1, 0x6B);
        ccWriteReg(m.csPin, CC1101_FREQ0, 0xD1);
    } else {
        // 869.524963 MHz (picture channel) with 26 MHz crystal: 0x21717A
        ccWriteReg(m.csPin, CC1101_FREQ2, 0x21);
        ccWriteReg(m.csPin, CC1101_FREQ1, 0x71);
        ccWriteReg(m.csPin, CC1101_FREQ0, 0x7A);
    }
}

/**
 * @brief Configure all CC1101 registers for the given module.
 */
static void initModuleRegisters(RadioModule& m) {
    const uint8_t cs = m.csPin;
    enterIdle(cs);

    // No GDO line is wired; IOCFG1 = 0x2E tri-states SO when CSn is HIGH (required on shared SPI)
    ccWriteReg(cs, CC1101_IOCFG2, 0x2E);
    ccWriteReg(cs, CC1101_IOCFG1, 0x2E);
    ccWriteReg(cs, CC1101_IOCFG0, 0x2E);

    // Intermediate Frequency (IF) and per-module crystal offset (FSCTRL0, 1.587 kHz/step)
    const GatewayConfig& cfg = ConfigManager::get();
    ccWriteReg(cs, CC1101_FSCTRL1, 0x06);
    ccWriteReg(cs, CC1101_FSCTRL0, (uint8_t)(m.index == 0 ? cfg.freqOffset : cfg.freqOffset2));

    writeModuleFreq(m);
    wdtFeed();

    // Modem Configuration: 38.383 kBaud, 2-FSK, 4B preamble, 30/32 sync bits
    ccWriteReg(cs, CC1101_MDMCFG4, 0x8A);
    ccWriteReg(cs, CC1101_MDMCFG3, 0x83);
    ccWriteReg(cs, CC1101_MDMCFG2, 0x03);
    ccWriteReg(cs, CC1101_MDMCFG1, 0x22);
    ccWriteReg(cs, CC1101_MDMCFG0, 0xF8);
    ccWriteReg(cs, CC1101_CHANNR,  0x00);
    ccWriteReg(cs, CC1101_DEVIATN, 0x35);   // 21.4 kHz deviation
    wdtFeed();

    // Sync Word: 0xD3 0x91
    ccWriteReg(cs, CC1101_SYNC1, 0xD3);
    ccWriteReg(cs, CC1101_SYNC0, 0x91);

    // Automatic Gain Control (AGC)
    ccWriteReg(cs, CC1101_AGCCTRL2, 0x43);
    ccWriteReg(cs, CC1101_AGCCTRL1, 0x40);
    ccWriteReg(cs, CC1101_AGCCTRL0, 0x95);
    wdtFeed();

    // Front-End configuration
    ccWriteReg(cs, CC1101_FREND1, 0x56);
    ccWriteReg(cs, CC1101_FREND0, 0x10);

    // Module 0 TX power ~+10 dBm (0xC0); module 1 is RX-only (0x00)
    uint8_t pa = m.canTx ? 0xC0 : 0x00;
    ccWriteBurst(cs, CC1101_PATABLE, &pa, 1);

    // Packet Configuration: fixed length RX_PKT_LEN (58) + status bytes, software CRC
    ccWriteReg(cs, CC1101_PKTLEN,   RX_PKT_LEN);
    ccWriteReg(cs, CC1101_PKTCTRL1, 0x04);   // Append RSSI and LQI
    ccWriteReg(cs, CC1101_PKTCTRL0, 0x00);   // Fixed length mode, CRC off
    wdtFeed();

    // State machine: Autocalibrate IDLE -> RX, Stay in RX after packet
    ccWriteReg(cs, CC1101_MCSM2,  0x07);
    ccWriteReg(cs, CC1101_MCSM1,  0x3C);   // 0x3C: RXOFF_MODE = Stay in RX
    ccWriteReg(cs, CC1101_MCSM0,  0x18);
    ccWriteReg(cs, CC1101_FOCCFG, 0x16);
    ccWriteReg(cs, CC1101_BSCFG,  0x6C);

    // Frequency Synthesizer Calibration
    ccWriteReg(cs, CC1101_FSCAL3, 0xE9);
    ccWriteReg(cs, CC1101_FSCAL2, 0x2A);
    ccWriteReg(cs, CC1101_FSCAL1, 0x00);
    ccWriteReg(cs, CC1101_FSCAL0, 0x1F);
    wdtFeed();

    // Hardware Test Registers (SWRS033 Table 61)
    ccWriteReg(cs, CC1101_TEST2, 0x81);
    ccWriteReg(cs, CC1101_TEST1, 0x35);
    ccWriteReg(cs, CC1101_TEST0, 0x09);

    enterRxMode(m);
}

// ---- queue helpers ----

static void pushItem(RadioModule& m, RadioFrame& f) {
    f.dropsBefore = s_pendingDrops;
    if (xQueueSend(s_rxQ, &f, 0) == pdTRUE) {
        s_pendingDrops = 0;
        uint32_t hw = (uint32_t)uxQueueMessagesWaiting(s_rxQ);
        maxTo(g_radioDiag.queueHighWater, hw);
        maxTo(g_radioDiag2.queueHighWater, hw);
    } else {
        bump(m.diag->queueDrops);
        if (s_pendingDrops < 255) s_pendingDrops++;
    }
}

static void postEvent(RadioModule& m, uint8_t kind, uint8_t arg0) {
    RadioFrame f;
    memset(&f, 0, sizeof(f));
    f.kind = kind;
    f.data[0] = arg0;
    f.module = m.index;
    f.channel = moduleChannel(m);
    f.flags = moduleFlags(m);
    f.startUs = f.doneUs = radioNowUs();
    pushItem(m, f);
}

/** FIFO overflow or stalled partial frame: report the bytes read so far and how many were lost. */
static void postLoss(RadioModule& m, uint8_t kind, uint8_t lost) {
    RadioFrame f;
    memset(&f, 0, sizeof(f));
    f.kind = kind;
    f.len = lost;
    memcpy(f.data, m.frame, m.frameLen);
    f.freqEst = m.frameFreqEst;
    f.startUs = m.frameStartUs;
    f.doneUs = radioNowUs();
    f.module = m.index;
    f.channel = moduleChannel(m);
    f.flags = moduleFlags(m);
    pushItem(m, f);
}

static void goOffline(RadioModule& m) {
    m.online = false;
    m.status->online = false;
    assemblerReset(m);
    m.gapValid = false;
    m.lastProbeMs = millis();
    postEvent(m, RADIO_ITEM_OFFLINE, 0);
}

static bool versionOk(uint8_t v) { return v == 0x04 || v == 0x14; }

static void markTxOverlap(RadioFrame& f) {
    if (f.module != 1) return;
    if (s_txActive) {
        if ((int32_t)(f.doneUs - s_txBeginUs) >= 0) f.flags |= RADIO_FLAG_TX_OVERLAP;
    } else if (s_hasLastTx && (uint32_t)(f.doneUs - s_lastTxEndUs) < 1000000UL) {
        if ((int32_t)(f.startUs - s_lastTxEndUs) <= 0 && (int32_t)(s_txBeginUs - f.doneUs) <= 0) {
            f.flags |= RADIO_FLAG_TX_OVERLAP;
        }
    }
}

// ---- RX service ----

static void radioServiceFifo(RadioModule& m, RadioFrame* completedOut, bool* hasCompleted) {
    if (hasCompleted) *hasCompleted = false;

    const uint32_t nowUs = radioNowUs();
    if (m.gapValid) {
        const uint32_t gap = (uint32_t)(nowUs - m.lastServiceUs);
        maxTo(m.diag->maxGapUs, gap);
        if (gap > 5000) bump(m.diag->gapOver5ms);
        if (gap > 10000) bump(m.diag->gapOver10ms);
        if (gap > 20000) bump(m.diag->gapOver20ms);
    }
    m.lastServiceUs = nowUs;
    m.gapValid = true;

    uint8_t rxBytesStatus = readStatusSafe(m, CC1101_RXBYTES);
    m.lastRxBytes = rxBytesStatus;
    if (m.stallSuspect) return;

    // Check for RX FIFO overflow (Bit 7 = 1)
    if (rxBytesStatus & 0x80) {
        bump(m.diag->fifoOverflows);
        postLoss(m, RADIO_ITEM_OVERFLOW, (uint8_t)(m.frameLen + (rxBytesStatus & 0x7F)));
        enterRxMode(m);
        assemblerReset(m);
        return;
    }

    uint8_t bytesInFifo = rxBytesStatus & 0x7F;
    uint8_t need = RX_FRAME_LEN - m.frameLen;
    // A frame (60 bytes) nearly fills the 64-byte FIFO, so it is read in pieces while it
    // comes in; the last byte is left in the FIFO until the frame is complete (errata).
    uint8_t take = bytesInFifo >= need ? need : (bytesInFifo > 1 ? bytesInFifo - 1 : 0);
    if (take) {
        if (m.frameLen == 0) {
            m.frameFreqEst = (int8_t)readStatusSafe(m, CC1101_FREQEST);
            m.frameStartUs = nowUs - (uint32_t)bytesInFifo * 208U;
        }
        if (!ccReadBurst(m.csPin, CC1101_RXFIFO, m.frame + m.frameLen, take)) {
            m.stallSuspect = true;
            return;
        }
        m.frameLen += take;
        m.partialStartUs = nowUs;
        m.partialSet = true;
    }

    if (m.frameLen == RX_FRAME_LEN) {
        RadioFrame f;
        memset(&f, 0, sizeof(f));
        memcpy(f.data, m.frame, RX_FRAME_LEN);
        f.kind = RADIO_ITEM_FRAME;
        f.len = RX_FRAME_LEN;
        f.freqEst = m.frameFreqEst;
        f.startUs = m.frameStartUs;
        f.doneUs = radioNowUs();
        f.module = m.index;
        f.channel = moduleChannel(m);
        f.flags = moduleFlags(m);
        markTxOverlap(f);
        bump(m.diag->frames);
        assemblerReset(m);

        if (completedOut && hasCompleted) {
            *completedOut = f;
            *hasCompleted = true;
        } else {
            pushItem(m, f);
        }

        // RXOFF_MODE = RX (MCSM1 0x3C): restart RX only if the radio left it
        if ((readStatusSafe(m, CC1101_MARCSTATE) & 0x1F) != 0x0D) enterRxMode(m);
    } else if (m.frameLen > 0 || bytesInFifo > 0) {
        if (!m.partialSet) {
            m.partialStartUs = nowUs;
            m.partialSet = true;
        }
        if ((uint32_t)(nowUs - m.partialStartUs) > RADIO_PARTIAL_TIMEOUT_MS * 1000UL) {
            bump(m.diag->partialDrops);
            postLoss(m, RADIO_ITEM_PARTIAL, (uint8_t)(m.frameLen + bytesInFifo));
            enterRxMode(m);
            assemblerReset(m);
        }
    } else {
        m.partialSet = false;
    }
}

/** RSSI + MARCSTATE sample + self-heal + periodic 300 s recalibration while idle. */
static void radioSampleStatus(RadioModule& m) {
    int raw = readStatusSafe(m, CC1101_RSSI, false);   // RSSI changes by nature: not "unstable"
    uint8_t marc = readStatusSafe(m, CC1101_MARCSTATE) & 0x1F;
    if (m.stallSuspect) return;
    m.status->rssiDbm = (int16_t)((raw >= 128) ? ((raw - 256) / 2) - 74 : (raw / 2) - 74);
    m.status->marcState = marc;
    m.status->sampledMs = millis();

    // Self-heal (never in TX here): IDLE, SLEEP/invalid, RXFIFO_OVERFLOW, TXFIFO_UNDERFLOW
    if (marc == 0x01 || marc == 0x00 || marc > 0x16) {
        if (!versionOk(ccReadStatus(m.csPin, CC1101_VERSION))) {
            goOffline(m);
        } else if (ccReadReg(m.csPin, CC1101_FREQ2) != 0x21 ||
                   ccReadReg(m.csPin, CC1101_FREQ1) != expectedFreq1(m)) {
            initModuleRegisters(m);
            bump(m.diag->rxReinits);
        } else {
            enterRxMode(m);
            bump(m.diag->rxRestarts);
        }
    } else if (marc == 0x11 || marc == 0x16) {
        enterRxMode(m);
        bump(m.diag->rxRestarts);
    } else if (marc == 0x0D && (uint32_t)(millis() - m.lastRecalMs) >= RADIO_RECAL_INTERVAL_MS) {
        enterRxMode(m);
    }
}

// ---- commands ----

/**
 * @brief Send one 24-byte packet on module 0 (TX is refused on module 1) and return to RX.
 *        Services module 1's FIFO in 1 ms steps during the ~7 ms TX wait.
 */
static void radioDoTx(RadioCmd& c) {
    if (c.module != 0 || !s_mod[0].canTx || !s_mod[0].online) return;
    RadioModule& m0 = s_mod[0];
    RadioModule& m1 = s_mod[1];

    uint32_t now = radioNowUs();
    if ((uint32_t)(now - c.frameStartUs) > RADIO_TX_MAX_AGE_MS * 1000UL) {
        bump(m0.diag->txLate);
        return;
    }
    if (m0.frameLen > 0) {
        bump(m0.diag->txAbortedRx);
        assemblerReset(m0);
    }
    enterIdle(m0.csPin);
    ccWriteReg(m0.csPin, CC1101_PKTLEN, VERISURE_PACKET_LEN);
    ccWriteReg(m0.csPin, CC1101_PKTCTRL0, 0x04);
    ccStrobe(m0.csPin, CC1101_SFTX);
    ccWriteBurst(m0.csPin, CC1101_TXFIFO, c.pkt, VERISURE_PACKET_LEN);
    ccStrobe(m0.csPin, CC1101_STX);
    bump(m0.status->txCount);
    const uint32_t stxUs = radioNowUs();
    maxTo(m0.diag->maxTxLatencyUs, (uint32_t)(stxUs - c.frameStartUs));

    s_txActive = true;
    s_txBeginUs = stxUs;

    bool done = false;
    while (!done && (uint32_t)(radioNowUs() - stxUs) < RADIO_TX_TIMEOUT_MS * 1000UL) {
        vTaskDelay(1);
        esp_task_wdt_reset();
        if (m1.online && !m1.stallSuspect) radioServiceFifo(m1, nullptr, nullptr);
        if ((uint32_t)(radioNowUs() - stxUs) >= 5000UL) {
            if ((readStatusSafe(m0, CC1101_MARCSTATE) & 0x1F) == 0x01) done = true;
        }
    }

    s_lastTxEndUs = radioNowUs();
    s_txActive = false;
    s_hasLastTx = true;

    if (done) bump(m0.diag->txDone); else bump(m0.diag->txTimeouts);
    enterIdle(m0.csPin);
    ccWriteReg(m0.csPin, CC1101_PKTLEN, RX_PKT_LEN);
    ccWriteReg(m0.csPin, CC1101_PKTCTRL0, 0x00);
    enterRxMode(m0);
    m0.gapValid = false;
}

/** CLI testspi for module 0 or 1: keeps the other module's CS HIGH throughout. */
static void radioDoTestSpi(uint8_t module) {
    if (module >= RADIO_NUM_MODULES) module = 0;
    RadioModule& m = s_mod[module];
    const uint8_t cs = m.csPin;
    const uint8_t otherCs = s_mod[1 - module].csPin;
    RadioTestSpiResult& r = s_testResult;

    pinMode(otherCs, OUTPUT);
    digitalWrite(otherCs, HIGH);
    pinMode(cs, OUTPUT);
    digitalWrite(cs, HIGH);
    pinMode(PIN_MISO, INPUT_PULLUP);

    delay(5);
    r.misoHigh = (uint8_t)digitalRead(PIN_MISO);

    digitalWrite(cs, LOW);
    delay(10);
    r.misoLow = (uint8_t)digitalRead(PIN_MISO);
    digitalWrite(cs, HIGH);

    radioSpiAttach();   // pinMode() above took MISO from the SPI peripheral (core 3.x)
    wdtFeed();

    // Manual CC1101 Power-On-Reset Sequence (TI Datasheet 19.1.2)
    digitalWrite(cs, HIGH);
    delayMicroseconds(50);
    digitalWrite(cs, LOW);
    delayMicroseconds(50);
    digitalWrite(cs, HIGH);
    delayMicroseconds(50);
    digitalWrite(cs, LOW);
    delay(5);

    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    r.sresSt = SPI.transfer(CC1101_SRES);
    digitalWrite(cs, HIGH);
    delay(10);
    wdtFeed();

    digitalWrite(cs, LOW);
    delayMicroseconds(50);
    r.partSt = SPI.transfer(CC1101_PARTNUM | 0xC0);
    r.part = SPI.transfer(0x00);
    digitalWrite(cs, HIGH);
    delayMicroseconds(20);

    digitalWrite(cs, LOW);
    delayMicroseconds(50);
    r.verSt = SPI.transfer(CC1101_VERSION | 0xC0);
    r.ver = SPI.transfer(0x00);
    digitalWrite(cs, HIGH);

    digitalWrite(cs, LOW);
    delayMicroseconds(50);
    r.marcSt = SPI.transfer(CC1101_MARCSTATE | 0xC0);
    r.marc = SPI.transfer(0x00);
    digitalWrite(cs, HIGH);
    SPI.endTransaction();

    wdtFeed();
    if (versionOk(r.ver)) {
        initModuleRegisters(m);
        m.status->version = r.ver;
        m.status->partnum = r.part;
        assemblerReset(m);
        m.gapValid = false;
        if (!m.online) {
            m.online = true;
            m.status->online = true;
            m.lastStatusMs = millis();
            postEvent(m, RADIO_ITEM_ONLINE, r.ver);
        }
    } else if (m.online) {
        goOffline(m);
    }
}

static void radioDoCommand(RadioCmd& c) {
    switch (c.type) {
        case RADIO_CMD_TX:
            radioDoTx(c);
            break;
        case RADIO_CMD_REINIT: {
            uint8_t idx = c.module < RADIO_NUM_MODULES ? c.module : 0;
            RadioModule& m = s_mod[idx];
            if (m.online) {
                if (versionOk(ccReadStatus(m.csPin, CC1101_VERSION))) {
                    initModuleRegisters(m);
                    assemblerReset(m);
                    m.gapValid = false;
                } else {
                    goOffline(m);
                }
            }
            s_doneSeq = c.seq;
            xSemaphoreGive(s_done);
            break;
        }
        case RADIO_CMD_TESTSPI:
            radioDoTestSpi(c.module);
            s_doneSeq = c.seq;
            xSemaphoreGive(s_done);
            break;
        case RADIO_CMD_SETMODE: {
            RadioModule& m = s_mod[1];
            m.calMode = (c.arg != 0);
            s_calMode = m.calMode;
            if (m.online) {
                if (versionOk(ccReadStatus(m.csPin, CC1101_VERSION))) {
                    writeModuleFreq(m);
                    assemblerReset(m);
                    enterRxMode(m);
                    m.gapValid = false;
                } else {
                    goOffline(m);
                }
            }
            s_doneSeq = c.seq;
            xSemaphoreGive(s_done);
            break;
        }
        default:
            break;
    }
}

// ---- the task ----

static void radioTaskMain(void*) {
    esp_task_wdt_add(nullptr);
    const uint32_t bootMs = millis();
    s_lastHwmMs = bootMs;
    for (int i = 0; i < RADIO_NUM_MODULES; i++) {
        s_mod[i].lastProbeMs = bootMs;
        s_mod[i].lastStatusMs = bootMs;
        s_mod[i].lastRecalMs = bootMs;
    }

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

        // 1. Service both online modules' FIFOs first; push completed frames in startUs order
        RadioFrame completed[RADIO_NUM_MODULES];
        bool hasFrame[RADIO_NUM_MODULES] = { false, false };
        for (int i = 0; i < RADIO_NUM_MODULES; i++) {
            if (s_mod[i].online) radioServiceFifo(s_mod[i], &completed[i], &hasFrame[i]);
        }
        if (hasFrame[0] && hasFrame[1]) {
            if ((int32_t)(completed[1].startUs - completed[0].startUs) < 0) {
                pushItem(s_mod[1], completed[1]);
                pushItem(s_mod[0], completed[0]);
            } else {
                pushItem(s_mod[0], completed[0]);
                pushItem(s_mod[1], completed[1]);
            }
        } else if (hasFrame[0]) {
            pushItem(s_mod[0], completed[0]);
        } else if (hasFrame[1]) {
            pushItem(s_mod[1], completed[1]);
        }

        // 1b. Per-module stall check & periodic RSSI/MARCSTATE/self-heal/recalibration
        for (int i = 0; i < RADIO_NUM_MODULES; i++) {
            RadioModule& m = s_mod[i];
            if (!m.online) continue;
            if (m.stallSuspect) {
                m.stallSuspect = false;
                if (!versionOk(ccReadStatus(m.csPin, CC1101_VERSION))) goOffline(m);
            } else if (m.frameLen == 0 && m.lastRxBytes == 0 &&
                       (uint32_t)(nowMs - m.lastStatusMs) >= RADIO_STATUS_INTERVAL_MS) {
                m.lastStatusMs = nowMs;
                radioSampleStatus(m);
            }
        }

        // 2. Commands (TX / REINIT / TESTSPI / SETMODE)
        RadioCmd c;
        while (xQueueReceive(s_cmdQ, &c, 0) == pdTRUE) radioDoCommand(c);

        // 3. Independent offline probing every 2 s per module (200 us timeout if absent)
        for (int i = 0; i < RADIO_NUM_MODULES; i++) {
            RadioModule& m = s_mod[i];
            if (!m.online && (uint32_t)(millis() - m.lastProbeMs) >= RADIO_PROBE_INTERVAL_MS) {
                m.lastProbeMs = millis();
                uint8_t ver = ccReadStatus(m.csPin, CC1101_VERSION);
                if (versionOk(ver)) {
                    initModuleRegisters(m);
                    m.status->version = ver;
                    m.status->partnum = ccReadStatus(m.csPin, CC1101_PARTNUM);
                    assemblerReset(m);
                    m.gapValid = false;
                    m.online = true;
                    m.status->online = true;
                    m.lastStatusMs = millis();
                    postEvent(m, RADIO_ITEM_ONLINE, ver);
                }
            }
        }

        if ((uint32_t)(nowMs - s_lastHwmMs) >= 1000) {
            s_lastHwmMs = nowMs;
            uint32_t hwm = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
            g_radioDiag.stackFreeMin = hwm;
            g_radioDiag2.stackFreeMin = hwm;
        }
        ulTaskNotifyTake(pdTRUE, RADIO_POLL_TICKS);
    }
}

// ---- public API ----

static bool initOneModule(RadioModule& m, uint8_t* verOut) {
    ccReset(m.csPin);
    delay(10);
    uint8_t ver = ccReadStatus(m.csPin, CC1101_VERSION);
    if (verOut) *verOut = ver;
    m.status->version = ver;
    if (!versionOk(ver)) return false;
    m.status->partnum = ccReadStatus(m.csPin, CC1101_PARTNUM);
    initModuleRegisters(m);
    return true;
}

bool radioInit(uint8_t* version, uint8_t* version2, bool* online2) {
    initModState();
    radioSpiAttach();
    bool on1 = initOneModule(s_mod[0], version);
    bool on2 = initOneModule(s_mod[1], version2);
    if (online2) *online2 = on2;
    return on1;
}

bool radioTaskStart(bool online, bool online2) {
    if (s_task) return true;
    QueueHandle_t rxQ = xQueueCreateWithCaps(RADIO_QUEUE_DEPTH, sizeof(RadioFrame), MALLOC_CAP_SPIRAM);
    if (!rxQ) return false;
    QueueHandle_t cmdQ = xQueueCreateStatic(RADIO_CMD_QUEUE_DEPTH, sizeof(RadioCmd), s_cmdQStore, &s_cmdQBuf);
    SemaphoreHandle_t done = xSemaphoreCreateBinaryStatic(&s_doneBuf);
    if (!cmdQ || !done) return false;
    s_rxQ = rxQ;
    s_cmdQ = cmdQ;
    s_done = done;
    s_mod[0].online = online;
    g_radioStatus.online = online;
    s_mod[1].online = online2;
    g_radioStatus2.online = online2;
    TaskHandle_t t = xTaskCreateStatic(radioTaskMain, "radio", sizeof(s_taskStack) / sizeof(StackType_t),
                                       nullptr, RADIO_TASK_PRIO, s_taskStack, &s_taskTcb);
    if (!t) {
        s_mod[0].online = s_mod[1].online = false;
        g_radioStatus.online = false;
        g_radioStatus2.online = false;
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
    memset(&c, 0, sizeof(c));
    c.type = RADIO_CMD_TX;
    c.module = 0;
    memcpy(c.pkt, pkt, VERISURE_PACKET_LEN);
    c.frameStartUs = frameStartUs;
    return sendCmd(c);
}

static bool sendAndWait(uint8_t type, uint8_t module, uint8_t arg, uint32_t waitMs) {
    if (!s_task || !s_cmdQ || !s_done) return false;
    xSemaphoreTake(s_done, 0);
    RadioCmd c;
    memset(&c, 0, sizeof(c));
    c.type = type;
    c.module = module;
    c.arg = arg;
    c.seq = ++s_nextSeq;
    if (!sendCmd(c)) return false;
    const uint32_t t0 = millis();
    for (;;) {
        const uint32_t elapsed = (uint32_t)(millis() - t0);
        const uint32_t remaining = elapsed < waitMs ? waitMs - elapsed : 0;
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(remaining)) != pdTRUE) return s_doneSeq == c.seq;
        if (s_doneSeq == c.seq) return true;
    }
}

bool radioRequestReinit(uint8_t module, uint32_t waitMs) {
    return sendAndWait(RADIO_CMD_REINIT, module < RADIO_NUM_MODULES ? module : 0, 0, waitMs);
}

bool radioRequestTestSpi(uint8_t module, RadioTestSpiResult* out, uint32_t waitMs) {
    if (!sendAndWait(RADIO_CMD_TESTSPI, module < RADIO_NUM_MODULES ? module : 0, 0, waitMs)) return false;
    if (out) *out = s_testResult;
    return true;
}

bool radioRequestSetMode(bool calMode, uint32_t waitMs) {
    return sendAndWait(RADIO_CMD_SETMODE, 1, calMode ? 1 : 0, waitMs);
}

bool radioIsCalMode() {
    return s_calMode;
}

void radioDiagReset() {
    memset(&g_radioLoopDiag, 0, sizeof(g_radioLoopDiag));
    s_resetReq = true;
    if (s_task) xTaskNotifyGive(s_task);
}

void radioDiagAppendJson(String& out, uint8_t module) {
    const RadioDiag& d = (module == 1) ? g_radioDiag2 : g_radioDiag;
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
    out += "\"stack_free\":" + String((unsigned long)d.stackFreeMin);
    if (module == 0) {
        const RadioLoopDiag& l = g_radioLoopDiag;
        out += ",\"poll_answers\":" + String((unsigned long)l.pollAnswers) + ",";
        out += "\"poll_late\":" + String((unsigned long)l.pollLate) + ",";
        out += "\"max_poll_answer_age_us\":" + String((unsigned long)l.maxPollAnswerAgeUs) + ",";
        out += "\"max_drain_gap_ms\":" + String((unsigned long)l.maxDrainGapMs);
    }
}

#ifdef RADIO_EVAL
void radioEvalStall(uint32_t ms) {
    s_evalStallMs = ms > 3000 ? 3000 : ms;
    if (s_task) xTaskNotifyGive(s_task);
}
#endif
