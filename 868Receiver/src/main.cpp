/**
 * @file main.cpp
 * @brief Verisure 868 MHz Sub-1GHz Gateway Firmware (ESP32-S2 & CC1101)
 *
 * Receives, decrypts, and authenticates 868.950 MHz radio packets from Verisure
 * sensors, and republishes telemetry to Home Assistant via MQTT with Auto-Discovery.
 *
 * ============================================================================
 * ARCHITECTURE & SUBSYSTEMS:
 * ============================================================================
 * 1. Hardware Transceiver (CC1101 SPI):
 *    - Frequency: 868.950 MHz (FREQ2=0x21, FREQ1=0x6B, FREQ0=0xD1 for 26 MHz CC1101 crystal)
 *    - Modulation: 2-FSK, 38.383 kBaud, 21.4 kHz frequency deviation
 *    - Sync Word: 0xD3 0x91 (30/32 bit correlation matching Verisure sensors)
 *    - Hardware SPI: Isolated ADC1 domain pins (GPIO 3, 5, 7, 9)
 *
 * 2. Cryptographic Security Engine (crypto.cpp):
 *    - AES-128-ECB hardware-accelerated decryption via mbedTLS
 *    - Key Derivation Function (KDF) from Master Key + Device ID
 *    - Message authentication ('VS' magic bytes) and Device ID echo check
 *    - 16-bit sequence counter anti-replay protection
 *
 * 3. Network & Integration Services:
 *    - Web Management Portal on Port 80 with Captive Portal DNS
 *    - MQTT Client with automatic reconnect & state publishing
 *    - Home Assistant MQTT Auto-Discovery for instantaneous integration
 *    - Serial CLI on USB-CDC (115200 baud)
 * ============================================================================
 */

#include <Arduino.h>
#include <SPI.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include "crypto.h"
#include "config_manager.h"
#include "sensor_registry.h"
#include "web_portal.h"
#include "serial_cli.h"
#include "mqtt_manager.h"
#include "image_receiver.h"
#include "downlink.h"
#include "ha_discovery.h"
#include <WiFi.h>

#include "hardware_pins.h"

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

/** @brief CRC-16 as the CC1101 computes it (poly 0x8005, init 0xFFFF, no whitening). */
static uint16_t ccCrc16(const uint8_t* d, size_t n) {
    uint16_t crc = 0xFFFF;
    while (n--) {
        uint8_t b = *d++;
        for (int i = 0; i < 8; i++) {
            if (((crc & 0x8000) >> 8) ^ (b & 0x80)) crc = (crc << 1) ^ 0x8005;
            else crc <<= 1;
            b <<= 1;
        }
    }
    return crc;
}

static bool crcAt(const uint8_t* f, size_t n) {
    return ccCrc16(f, n) == (((uint16_t)f[n] << 8) | f[n + 1]);
}

unsigned long lastEventTime = 0;
unsigned long lastHeartbeat = 0;
unsigned long packetCount   = 0;

/**
 * @brief Print elapsed uptime timestamp and delta milliseconds to Serial monitor.
 * @param now Current uptime in milliseconds (millis()).
 */
void printTimestamp(unsigned long now) {
    unsigned long delta = (lastEventTime > 0) ? (now - lastEventTime) : 0;
    lastEventTime = now;
    Serial.printf("[%4lu.%03lu s] (+%5lu ms) ", now / 1000, now % 1000, delta);
}

/**
 * @brief Safely read a CC1101 status register with de-glitching / double-sampling.
 * @param addr CC1101 status register address.
 * @return Stabilized register value.
 * @note Required by TI CC1101 Errata note when reading status registers while SPI clock is running.
 */
byte readStatusSafe(byte addr) {
    byte val1 = ELECHOUSE_cc1101.SpiReadStatus(addr);
    byte val2 = ELECHOUSE_cc1101.SpiReadStatus(addr);
    while (val1 != val2) {
        val1 = val2;
        val2 = ELECHOUSE_cc1101.SpiReadStatus(addr);
    }
    return val1;
}

/**
 * @brief Reset RX FIFO and put CC1101 into continuous receive mode (SRX).
 */
void enterRxMode() {
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);
    delayMicroseconds(100);
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SFRX);
    delayMicroseconds(100);
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SRX);
}

static bool s_radioOnline = false;

/**
 * @brief Send one 24-byte packet and return to RX (MCSM1 TXOFF_MODE = IDLE).
 */
static void radioTransmit(const uint8_t* pkt) {
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);
    // the camera receives 24 bytes with hardware CRC (RX here uses 58 bytes, CRC off)
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_PKTLEN, VERISURE_PACKET_LEN);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_PKTCTRL0, 0x04);
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SFTX);
    ELECHOUSE_cc1101.SpiWriteBurstReg(CC1101_TXFIFO, (byte*)pkt, VERISURE_PACKET_LEN);
    ELECHOUSE_cc1101.SpiStrobe(CC1101_STX);
    unsigned long t0 = millis();
    while ((readStatusSafe(CC1101_MARCSTATE) & 0x1F) != 0x01 && millis() - t0 < 30) {
        delayMicroseconds(200);
    }
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_PKTLEN, RX_PKT_LEN);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_PKTCTRL0, 0x00);
    enterRxMode();
}

/**
 * @brief Answer a camera poll (see downlink.h) with the alarm state and any queued
 *        picture command. Called right after the poll was authenticated, because the
 *        camera only listens for ~0.1 s.
 */
/** Gateway frequency correction in FSCTRL0 steps of 1.587 kHz (see initRadioRegisters). */
#ifndef RX_FREQ_OFFSET
#define RX_FREQ_OFFSET 38
#endif

static void answerCameraPoll(const VerisureTelemetry& t) {
    uint8_t key[16], plain[16], pkt[VERISURE_PACKET_LEN];
    uint8_t state = t.rawP1;             // plain[13] of the poll
    bool armed = downlinkArmed();

    // only polls with the persistent counter: the answer echoes it, which makes it unique
    if (!t.hasCounter) {
        Serial.printf("[Downlink] %s polls without the persistent counter: not answered (old camera firmware?)\n",
                      t.deviceId);
        return;
    }
    if (!cryptoDeriveDeviceKey(ConfigManager::get().masterKey, t.deviceId, key)) return;
    uint16_t first = 0;
    uint32_t map = 0;
    imageReceiverPollSeen(t.deviceId);   // may queue a resend for a picture that lost its end
    uint8_t cmd = downlinkCommandForPoll(t.deviceId, state, &first, &map);
    downlinkBuildPlain(t.counter, armed, cmd, first, map, plain);
    memcpy(pkt, t.deviceId, 8);
    bool ok = cryptoEncryptPayload(key, plain, &pkt[8]);
    memset(key, 0, sizeof(key));
    if (!ok) return;
    radioTransmit(pkt);
    Serial.printf("[Downlink] -> %s: armed %d, cmd 0x%02X (poll %lu, camera state 0x%02X)",
                  t.deviceId, armed ? 1 : 0, cmd, (unsigned long)t.counter, state);
    if (cmd == DOWNLINK_CMD_RESEND)
        Serial.printf(", resend %d chunks from %u (map %08lX)", __builtin_popcount(map), first,
                      (unsigned long)map);
    Serial.println();
    if (((state & DOWNLINK_STATE_ARMED) != 0) != armed) {
        Serial.printf("[Downlink] NOTE: %s reported armed %d before this answer\n",
                      t.deviceId, (state & DOWNLINK_STATE_ARMED) ? 1 : 0);
    }
}

/**
 * @brief Configure all CC1101 registers to match the Verisure 868.950 MHz radio profile.
 */
void initRadioRegisters() {
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);

    // Intermediate Frequency (IF)
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FSCTRL1,  0x06);
    // Frequency offset (FSCTRL0, f_xosc / 2^14 = 1.587 kHz per step, RX and TX): this CC1101
    // module's crystal runs low, the sensors arrived ~+60 kHz above the channel (FREQEST, logged
    // as "Freq. error" per packet). That is beyond the frequency loop's +-50 kHz (FOCCFG
    // FOC_LIMIT BW/4), so weak packets failed their CRC (2026-11-17). Re-measure for another
    // module: the logged error should stay near 0.
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FSCTRL0,  (byte)RX_FREQ_OFFSET);

    // RF Frequency: 868.950 MHz (with 26.000 MHz crystal on standard CC1101 modules)
    // Formula: FREQ = (868.950 * 2^16) / 26.0 = 2190289 = 0x216BD1
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FREQ2,    0x21);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FREQ1,    0x6B);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FREQ0,    0xD1);

    // Modem Configuration: 38.383 kBaud, 2-FSK, 4B preamble, 30/32 sync bits
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_MDMCFG4,  0x8A);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_MDMCFG3,  0x83);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_MDMCFG2,  0x03);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_MDMCFG1,  0x22);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_MDMCFG0,  0xF8);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_CHANNR,   0x00);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_DEVIATN,  0x35); // 21.4 kHz deviation

    // Sync Word: 0xD3 0x91
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_SYNC1,    0xD3);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_SYNC0,    0x91);

    // Automatic Gain Control (AGC)
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_AGCCTRL2, 0x43);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_AGCCTRL1, 0x40);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_AGCCTRL0, 0x95);

    // Front-End configuration
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FREND1,   0x56);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FREND0,   0x10);

    // TX power for the camera downlink (PATABLE[0], ~+10 dBm at 868 MHz)
    byte pa = 0xC0;
    ELECHOUSE_cc1101.SpiWriteBurstReg(CC1101_PATABLE, &pa, 1);

    // Packet Configuration: fixed length RX_PKT_LEN (58) + status bytes, CRC checked in
    // software (see RX_FRAME_LEN); radioTransmit switches to 24 bytes with CRC for the downlink
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_PKTLEN,   RX_PKT_LEN);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_PKTCTRL1, 0x04); // Append RSSI and LQI
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_PKTCTRL0, 0x00); // Fixed length mode, CRC off

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

    // Hardware Test Registers (SWRS033 Table 61)
    ELECHOUSE_cc1101.SpiWriteReg(0x2C,            0x81);
    ELECHOUSE_cc1101.SpiWriteReg(0x2D,            0x35);
    ELECHOUSE_cc1101.SpiWriteReg(0x2E,            0x09);

    enterRxMode();
}

void setup() {
    Serial.begin(115200);
    delay(2000);

    Serial.println("\n=======================================================");
    Serial.println("  ESP32-S2 VERISURE GATEWAY & RECEIVER (868.950 MHz)   ");
    Serial.println("=======================================================\n");

    // 1. Initialize WebPortal, NVS Config, Serial CLI & MQTT
    webPortalInit();
    downlinkInit();
    serialCliInit();
    mqttInit();

    Serial.println("[System] Allowing Wi-Fi time to establish connection before radio start...");
    unsigned long wifiWaitStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiWaitStart < 8000) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[Wi-Fi] Connected! IP: %s | RSSI: %d dBm\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
        Serial.println("[Wi-Fi] Connecting in background...");
    }

    // 2. Initialize CC1101 SPI & Radio Transceiver on ADC1 pins
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
    ELECHOUSE_cc1101.setSpiPin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
    ELECHOUSE_cc1101.Init();
    delay(50);
    byte ver = ELECHOUSE_cc1101.SpiReadStatus(CC1101_VERSION);

    if (ver == 0x04 || ver == 0x14) {
        s_radioOnline = true;
        Serial.printf("  [CC1101] SPI OK! Chip Version: 0x%02X (SCK=%d, MISO=%d, MOSI=%d, CS=%d)\n",
                      ver, PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
        initRadioRegisters();
        printTimestamp(millis());
        Serial.println("[System] Receiver listening on 868.950 MHz (Robust RX, 30/32 Sync)...");
    } else {
        s_radioOnline = false;
        Serial.printf("  [CC1101] No communication (read 0x%02X on SCK=%d, MISO=%d, MOSI=%d, CS=%d).\n",
                      ver, PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
        Serial.println("  [CC1101] CC1101 disconnected - waiting for connection on pins (GPIO 3, 5, 7, 9).\n");
    }

    lastEventTime = millis();
    lastHeartbeat = lastEventTime;
}

void loop() {
    unsigned long now = millis();
    static int cachedRssi = -100;
    static byte cachedMarc = 0x01;

    // Heartbeat every 2 seconds
    if (now - lastHeartbeat >= 2000) {
        lastHeartbeat = now;
        if (s_radioOnline) {
            cachedRssi = ELECHOUSE_cc1101.getRssi();
            cachedMarc = readStatusSafe(CC1101_MARCSTATE) & 0x1F;
        } else {
            byte ver = ELECHOUSE_cc1101.SpiReadStatus(CC1101_VERSION);
            if (ver == 0x04 || ver == 0x14) {
                s_radioOnline = true;
                Serial.printf("\n[CC1101] MODULE DETECTED! Version: 0x%02X. Initializing radio...\n", ver);
                initRadioRegisters();
            }
        }
        Serial.printf("[Gateway Heartbeat] Wi-Fi: %s | MQTT: %s | CC1101: %s | RSSI: %d dBm | Packets: %lu\n",
                      WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "Connecting...",
                      mqttGetStatusString().c_str(),
                      s_radioOnline ? "Online (868.95 MHz)" : "Disconnected",
                      s_radioOnline ? cachedRssi : 0,
                      packetCount);
    }

    // 1. Process Background Services (Non-blocking)
    webPortalLoop(cachedRssi, cachedMarc, packetCount);
    serialCliLoop();
    mqttLoop(cachedRssi, cachedMarc, packetCount);
    imageReceiverLoop();

    // 2. Process Radio Receiver FIFO
    if (!s_radioOnline) {
        delay(10);
        return;
    }

    byte rxBytesStatus = readStatusSafe(CC1101_RXBYTES);
    static byte frame[RX_FRAME_LEN];
    static byte frameLen = 0;
    static unsigned long partialStart = 0;
    static int8_t frameFreqEst = 0;   // FREQEST at the start of the frame (see below)

    // Check for RX FIFO overflow (Bit 7 = 1)
    if (rxBytesStatus & 0x80) {
        enterRxMode();
        frameLen = 0;
        partialStart = 0;
    } else {
        byte bytesInFifo = rxBytesStatus & 0x7F;
        byte need = RX_FRAME_LEN - frameLen;
        // A frame (60 bytes) nearly fills the 64-byte FIFO, so it is read in pieces while it
        // comes in; the last byte is left in the FIFO until the frame is complete (errata).
        byte take = bytesInFifo >= need ? need : (bytesInFifo > 1 ? bytesInFifo - 1 : 0);
        if (take) {
            // FREQEST is read while the first bytes come in: the frame is 58 bytes, so after a
            // 24-byte packet the frequency loop tracks noise for ~7 ms and drifts off
            if (frameLen == 0) frameFreqEst = (int8_t)readStatusSafe(CC1101_FREQEST);
            ELECHOUSE_cc1101.SpiReadBurstReg(CC1101_RXFIFO, frame + frameLen, take);
            frameLen += take;
            partialStart = now;
        }

        if (frameLen == RX_FRAME_LEN) {
            frameLen = 0;
            partialStart = 0;
            byte rawRssi = frame[RX_PKT_LEN];
            byte rawLqi  = frame[RX_PKT_LEN + 1];
            // Frequency offset the CC1101 estimated for this packet (FREQEST, two's complement,
            // f_xosc / 2^14 = 1.587 kHz per step), read at the start of the frame
            float freqErrKhz = frameFreqEst * (26000.0f / 16384.0f);

            // Convert raw RSSI byte to dBm (CC1101 datasheet section 17.3)
            int packetRssi = (rawRssi >= 128) ? ((rawRssi - 256) / 2) - 74 : (rawRssi / 2) - 74;
            const uint8_t* activeMasterKey = ConfigManager::get().masterKey;

            if (crcAt(frame, BIG_PACKET_LEN) && cryptoProcessBigPacket(frame, activeMasterKey)) {
                // camera image packet (type 0x82), handled by the image receiver
            } else if (crcAt(frame, VERISURE_PACKET_LEN)) {
                VerisureTelemetry telem;
                if (cryptoProcessPacket(frame, packetRssi, rawLqi & 0x7F, activeMasterKey, &telem)) {
                    if (telem.rawP0 == DOWNLINK_POLL_MARKER && haIsMotionCameraSensor(telem.deviceId)) {
                        answerCameraPoll(telem);
                    }
                    packetCount++;
                    sensorRegistryUpdate(telem); // Cache in memory
                    mqttPublishTelemetry(telem); // Publish to Home Assistant MQTT
                    printTimestamp(now);
                    Serial.printf(">>> [#%04lu] [AUTHENTICATED] Device: \"%s\" | Seq: #%lu <<<\n",
                                  packetCount, telem.deviceId, (unsigned long)telem.counter);
                    Serial.printf("    Magnet  : %s\n", telem.isOpen ? "OPEN [ALARM!]" : "CLOSED (Secure)");
                    Serial.printf("    Shock   : %s\n", telem.isShock ? "SHOCK / VIBRATION DETECTED!" : "Clear");
                    Serial.printf("    Tamper  : %s\n", telem.isButton ? "TAMPER (Cover removed!)" : "Secure (Cover closed)");
                    Serial.printf("    Battery : %u mV (%u%%)%s\n", telem.batteryMv, telem.batteryPct, telem.isLowBatt ? " [LOW BATTERY!]" : "");
                    Serial.printf("    Signal  : RSSI %d dBm | LQI %u | Freq. error %+.1f kHz | CRC: OK\n",
                                  telem.rssi, telem.lqi, freqErrKhz);
                    Serial.printf("    Pins    : P0=0x%02X P1=0x%02X P2=0x%02X (P2_0=%d) RST_CAUSE=%u\n\n",
                                  telem.rawP0, telem.rawP1, telem.rawP2, telem.rawP2 & 0x01, telem.rawRst);
                } else {
                    printTimestamp(now);
                    Serial.println("[Security] Packet received but rejected (invalid key, spoofing, or replay attack).");
                }
            } else {
                printTimestamp(now);
                Serial.printf("[Radio] Packet received with invalid CRC (dropped): RSSI %d dBm, freq. error %+.1f kHz\n",
                              packetRssi, freqErrKhz);
            }
            // RXOFF_MODE = RX (MCSM1 0x3C): the radio is already receiving the next packet.
            // A flush here cut it off; with picture chunks back to back that lost a third of
            // them. Restart RX only if the radio left it (e.g. after the poll answer's TX).
            if ((readStatusSafe(CC1101_MARCSTATE) & 0x1F) != 0x0D) enterRxMode();
        } else if (frameLen > 0 || bytesInFifo > 0) {
            // a frame that stops coming in: drop it and restart RX
            if (partialStart == 0) partialStart = now;
            if (now - partialStart > 50) {
                printTimestamp(now);
                Serial.printf("[Partial %d B] dropped\n", frameLen + bytesInFifo);
                enterRxMode();
                frameLen = 0;
                partialStart = 0;
            }
        } else {
            partialStart = 0;
        }
    }

    delay(2);
}
