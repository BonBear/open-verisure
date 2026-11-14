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
#include <WiFi.h>

#include "hardware_pins.h"

/** @brief Length of sensor telemetry payload: 8B Header + 16B AES Ciphertext. */
#define VERISURE_PACKET_LEN 24

/** @brief Total bytes read from RX FIFO including CC1101 RSSI and LQI/CRC status bytes. */
#define TOTAL_RX_LEN        (VERISURE_PACKET_LEN + 2)

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
 * @brief Configure all CC1101 registers to match the Verisure 868.950 MHz radio profile.
 */
void initRadioRegisters() {
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);

    // Intermediate Frequency (IF)
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FSCTRL1,  0x06);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FSCTRL0,  0x00);

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
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_AGCCTRL0, 0x91);

    // Front-End configuration
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FREND1,   0x56);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_FREND0,   0x10);

    // Packet Configuration: Fixed length 24 bytes + status bytes
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_PKTLEN,   VERISURE_PACKET_LEN);
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_PKTCTRL1, 0x04); // Append RSSI and LQI/CRC
    ELECHOUSE_cc1101.SpiWriteReg(CC1101_PKTCTRL0, 0x04); // Fixed length mode, CRC enabled

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
    static unsigned long partialStart = 0;

    // Check for RX FIFO overflow (Bit 7 = 1)
    if (rxBytesStatus & 0x80) {
        enterRxMode();
        partialStart = 0;
    } else {
        byte bytesInFifo = rxBytesStatus & 0x7F;

        // Complete packet available in FIFO (24 bytes payload + 2 bytes status)
        if (bytesInFifo >= TOTAL_RX_LEN) {
            partialStart = 0;
            byte rxBuffer[TOTAL_RX_LEN] = {0};
            ELECHOUSE_cc1101.SpiReadBurstReg(CC1101_RXFIFO, rxBuffer, TOTAL_RX_LEN);

            byte rawRssi = rxBuffer[VERISURE_PACKET_LEN];
            byte rawLqi  = rxBuffer[VERISURE_PACKET_LEN + 1];

            // Convert raw RSSI byte to dBm (CC1101 datasheet section 17.3)
            int packetRssi = (rawRssi >= 128) ? ((rawRssi - 256) / 2) - 74 : (rawRssi / 2) - 74;
            bool crcOk = (rawLqi & 0x80) != 0;

            if (crcOk) {
                VerisureTelemetry telem;
                const uint8_t* activeMasterKey = ConfigManager::get().masterKey;
                if (cryptoProcessPacket(rxBuffer, packetRssi, rawLqi & 0x7F, activeMasterKey, &telem)) {
                    packetCount++;
                    sensorRegistryUpdate(telem); // Cache in memory
                    mqttPublishTelemetry(telem); // Publish to Home Assistant MQTT
                    printTimestamp(now);
                    Serial.printf(">>> [#%04lu] [AUTHENTICATED] Device: \"%s\" | Seq: #%u <<<\n",
                                  packetCount, telem.deviceId, telem.sequence);
                    Serial.printf("    Magnet  : %s\n", telem.isOpen ? "OPEN [ALARM!]" : "CLOSED (Secure)");
                    Serial.printf("    Shock   : %s\n", telem.isShock ? "SHOCK / VIBRATION DETECTED!" : "Clear");
                    Serial.printf("    Tamper  : %s\n", telem.isButton ? "TAMPER (Cover removed!)" : "Secure (Cover closed)");
                    Serial.printf("    Battery : %u mV (%u%%)%s\n", telem.batteryMv, telem.batteryPct, telem.isLowBatt ? " [LOW BATTERY!]" : "");
                    Serial.printf("    Signal  : RSSI %d dBm | LQI %u | CRC: OK\n", telem.rssi, telem.lqi);
                    Serial.printf("    Pins    : P0=0x%02X P1=0x%02X P2=0x%02X (P2_0=%d) RST_CAUSE=%u\n\n",
                                  telem.rawP0, telem.rawP1, telem.rawP2, telem.rawP2 & 0x01, telem.rawRst);
                } else if (!cryptoLastPacketWasImage()) {   // camera packets are handled, not rejected
                    printTimestamp(now);
                    Serial.println("[Security] Packet received but rejected (invalid key, spoofing, or replay attack).");
                }
            } else {
                printTimestamp(now);
                Serial.println("[Radio] Packet received with invalid hardware CRC (dropped).");
            }
            enterRxMode();
        } else if (bytesInFifo > 0) {
            // Partial packet timeout handling
            if (partialStart == 0) partialStart = now;
            if (now - partialStart > 50) {
                byte pBuf[32] = {0};
                byte toRead = (bytesInFifo > 32) ? 32 : bytesInFifo;
                ELECHOUSE_cc1101.SpiReadBurstReg(CC1101_RXFIFO, pBuf, toRead);
                printTimestamp(now);
                Serial.printf("[Partial %d B] ", toRead);
                for (byte b = 0; b < toRead; b++) Serial.printf("%02X ", pBuf[b]);
                Serial.println();
                enterRxMode();
                partialStart = 0;
            }
        } else {
            partialStart = 0;
        }
    }

    delay(2);
}
