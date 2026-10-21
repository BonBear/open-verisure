/**
 * @file serial_cli.cpp
 * @brief Interactive Serial Command Line Interface Implementation
 */

#include "serial_cli.h"
#include "config_manager.h"
#include "sensor_registry.h"
#include "web_portal.h"
#include "hardware_pins.h"
#include "mqtt_manager.h"
#include <WiFi.h>
#include <SPI.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include "esp_wifi.h"

static char s_cmdBuf[128];
static int s_cmdLen = 0;

void serialCliInit() {
    s_cmdLen = 0;
}

static void printHelp() {
    Serial.println("\n--- VERISURE GATEWAY SERIAL CLI ---");
    Serial.println("  help                       - Visa detta hjälpmeddelande");
    Serial.println("  status                     - Visa aktuell gateway- och sensorstatus");
    Serial.println("  testspi                    - Testa SPI-kontakt och register på CC1101");
    Serial.println("  scan                       - Skanna och lista tillgängliga Wi-Fi-nätverk");
    Serial.println("  connect <ssid> [losenord]  - Anslut till Wi-Fi och spara till NVS");
    Serial.println("  setkey <32-hex-tecken>     - Konfigurera Master AES-nyckel");
    Serial.println("  setmqtt <host> <port> [user] [pass] [topic] - Konfigurera MQTT-broker");
    Serial.println("  reset                      - Rensa alla NVS-inställningar");
    Serial.println("  reboot                     - Starta om gatewayn");
    Serial.println("-----------------------------------\n");
}

static void runTestSpi() {
    Serial.println("\n--- SPI & CC1101 DIAGNOSTIK ---");
    Serial.printf("Pinnar: SCK=GPIO%d, MISO=GPIO%d, MOSI=GPIO%d, CS=GPIO%d\n",
                  PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);

    pinMode(PIN_CS, OUTPUT);
    pinMode(PIN_MISO, INPUT_PULLUP);

    digitalWrite(PIN_CS, HIGH);
    delay(5);
    int miso_high = digitalRead(PIN_MISO);

    digitalWrite(PIN_CS, LOW);
    delay(10);
    int miso_low = digitalRead(PIN_MISO);
    digitalWrite(PIN_CS, HIGH);

    Serial.printf("CS HIGH (med Pullup) -> MISO: %d (Bör vara 1 när CC1101 är avstängd)\n", miso_high);
    Serial.printf("CS LOW                -> MISO: %d (MÅSTE bli 0 när CC1101 svarar!)\n", miso_low);

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
    byte sres_st = SPI.transfer(0x30); // CC1101_SRES
    digitalWrite(PIN_CS, HIGH);
    delay(10);

    // Read PARTNUM (0x30 | 0xC0 = 0xF0) and VERSION (0x31 | 0xC0 = 0xF1)
    digitalWrite(PIN_CS, LOW);
    delayMicroseconds(50);
    byte st_part = SPI.transfer(0x30 | 0xC0);
    byte val_part = SPI.transfer(0x00);
    digitalWrite(PIN_CS, HIGH);
    delayMicroseconds(20);

    digitalWrite(PIN_CS, LOW);
    delayMicroseconds(50);
    byte st_ver = SPI.transfer(0x31 | 0xC0);
    byte val_ver = SPI.transfer(0x00);
    digitalWrite(PIN_CS, HIGH);

    // Read MARCSTATE (0x35 | 0xC0 = 0xF5)
    digitalWrite(PIN_CS, LOW);
    delayMicroseconds(50);
    byte st_marc = SPI.transfer(0x35 | 0xC0);
    byte val_marc = SPI.transfer(0x00);
    digitalWrite(PIN_CS, HIGH);
    SPI.endTransaction();

    Serial.printf("Efter SRES Reset:\n");
    Serial.printf("  SRES Status: 0x%02X (CHIP_RDYn=%d, STATE=%d)\n", sres_st, (sres_st >> 7) & 1, (sres_st >> 4) & 7);
    Serial.printf("  PARTNUM    : 0x%02X (Status: 0x%02X, förväntat: 0x00)\n", val_part, st_part);
    Serial.printf("  VERSION    : 0x%02X (Status: 0x%02X, förväntat: 0x04 eller 0x14)\n", val_ver, st_ver);
    Serial.printf("  MARCSTATE  : 0x%02X (Status: 0x%02X, 0x01=IDLE, 0x0D=RX)\n", val_marc, st_marc);

    if (val_ver == 0x04 || val_ver == 0x14) {
        Serial.println("[+] SPI och CC1101 fungerar UTMÄRKT!\n");
    } else {
        Serial.println("[-] FEL: CC1101 svarar inte korrekt via SPI.\n");
    }
}

static void printStatus() {
    const GatewayConfig& cfg = ConfigManager::get();
    Serial.println("\n================ GATEWAY STATUS ================");
    Serial.printf("  Uptime       : %lu sekunder\n", millis() / 1000);
    Serial.printf("  Wi-Fi Läge   : %s\n", webPortalIsApMode() ? "Access Point (AP)" : "Station (STA)");
    Serial.printf("  IP-Adress    : %s\n", webPortalGetIp().c_str());
    Serial.printf("  Wi-Fi SSID   : %s (Signal: %d dBm)\n",
                  webPortalIsApMode() ? "Verisure-Gateway" : cfg.wifiSsid,
                  webPortalIsApMode() ? 0 : WiFi.RSSI());
    Serial.printf("  MQTT Status  : %s\n", mqttGetStatusString().c_str());
    Serial.printf("  Master Key   : %s\n", cfg.masterKeyHex);
    Serial.printf("  Sensorer     : %d st registrerade\n", sensorRegistryGetCount());

    int count = sensorRegistryGetCount();
    for (int i = 0; i < count; i++) {
        const SensorRecord* rec = sensorRegistryGetByIndex(i);
        if (!rec) continue;
        Serial.printf("    - ID: %s | Magnet: %s | Skak: %s | Sabotage: %s | Batt: %u mV (%u%%) | RSSI: %d dBm | Paket: %u\n",
                      rec->lastTelem.deviceId,
                      rec->lastTelem.isOpen ? "ÖPPEN [LARM]" : "STÄNGD",
                      rec->lastTelem.isShock ? "SKAK!" : "Lugnt",
                      rec->lastTelem.isButton ? "SABOTAGE!" : "Säkrad",
                      rec->lastTelem.batteryMv, rec->lastTelem.batteryPct,
                      rec->lastTelem.rssi, rec->packetCount);
    }
    Serial.println("================================================\n");
}

static void runScan() {
    Serial.println("[Wi-Fi] Skannar efter nätverk...");
    WiFi.disconnect(true);
    delay(200);
    WiFi.mode(WIFI_STA);
    delay(100);
    int n = WiFi.scanNetworks(false, true);
    Serial.printf("[Wi-Fi] Hittade %d nätverk:\n", n);
    for (int i = 0; i < n; i++) {
        wifi_auth_mode_t auth = WiFi.encryptionType(i);
        const char* authStr = (auth == WIFI_AUTH_OPEN) ? "Öppet" :
                              (auth == WIFI_AUTH_WPA_PSK) ? "WPA" :
                              (auth == WIFI_AUTH_WPA2_PSK) ? "WPA2" :
                              (auth == WIFI_AUTH_WPA_WPA2_PSK) ? "WPA/WPA2" :
                              (auth == WIFI_AUTH_WPA2_ENTERPRISE) ? "Enterprise" :
                              (auth == WIFI_AUTH_WPA3_PSK) ? "WPA3" :
                              (auth == WIFI_AUTH_WPA2_WPA3_PSK) ? "WPA2/WPA3" : "Krypterat";
        Serial.printf("  %2d: %-20s (Ch %2d, %3d dBm) [%s] BSSID: %s\n",
                      i + 1,
                      WiFi.SSID(i).c_str(),
                      WiFi.channel(i),
                      WiFi.RSSI(i),
                      authStr,
                      WiFi.BSSIDstr(i).c_str());
    }
    WiFi.scanDelete();
}

static void processCommand(char* cmd) {
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (*cmd == '\0') return;

    if (strcmp(cmd, "help") == 0) {
        printHelp();
    } else if (strcmp(cmd, "status") == 0) {
        printStatus();
    } else if (strcmp(cmd, "testspi") == 0) {
        runTestSpi();
    } else if (strcmp(cmd, "scan") == 0) {
        runScan();
    } else if (strncmp(cmd, "connect ", 8) == 0) {
        char* args = cmd + 8;
        while (*args == ' ') args++;
        char* space = strchr(args, ' ');
        char ssid[33] = {0};
        char pass[65] = {0};
        if (space) {
            *space = '\0';
            strncpy(ssid, args, sizeof(ssid) - 1);
            char* p = space + 1;
            while (*p == ' ') p++;
            strncpy(pass, p, sizeof(pass) - 1);
        } else {
            strncpy(ssid, args, sizeof(ssid) - 1);
        }

        Serial.printf("[CLI] Ansluter till SSID \"%s\"...\n", ssid);
        WiFi.disconnect(true, true);
        delay(200);
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid, pass);

        unsigned long t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
            delay(250);
            Serial.print(".");
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[CLI] Ansluten! IP: %s | RSSI: %d dBm\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
            ConfigManager::setWifi(ssid, pass);
            ConfigManager::save();
        } else {
            Serial.println("[CLI] Anslutningen misslyckades.");
        }
    } else if (strncmp(cmd, "setkey ", 7) == 0) {
        char* keyStr = cmd + 7;
        while (*keyStr == ' ') keyStr++;
        if (strlen(keyStr) == 32 && ConfigManager::setMasterKeyHex(keyStr)) {
            ConfigManager::save();
            Serial.printf("[CLI] Master Key sparad: %s\n", keyStr);
        } else {
            Serial.println("[CLI] Fel: Nyckeln måste vara exakt 32 hex-tecken!");
        }
    } else if (strncmp(cmd, "setmqtt ", 8) == 0) {
        char host[65] = {0};
        int port = 1883;
        char user[33] = {0};
        char pass[65] = {0};
        char topic[33] = "verisure";
        int parsed = sscanf(cmd + 8, "%64s %d %32s %64s %32s", host, &port, user, pass, topic);
        if (parsed >= 2) {
            ConfigManager::setMqtt(host, (uint16_t)port, user, pass, topic);
            ConfigManager::save();
            Serial.printf("[CLI] MQTT sparad: %s:%d (Topic: %s)\n", host, port, topic);
        } else {
            Serial.println("[CLI] Användning: setmqtt <host> <port> [user] [pass] [topic]");
        }
    } else if (strcmp(cmd, "reset") == 0) {
        ConfigManager::resetToDefaults();
        Serial.println("[CLI] Återställt alla inställningar till standard.");
    } else if (strcmp(cmd, "reboot") == 0) {
        Serial.println("[CLI] Startar om gatewayn nu...");
        delay(100);
        ESP.restart();
    } else {
        Serial.printf("[CLI] Okänt kommando: \"%s\". Skriv 'help' för lista.\n", cmd);
    }
}

void serialCliLoop() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            s_cmdBuf[s_cmdLen] = '\0';
            processCommand(s_cmdBuf);
            s_cmdLen = 0;
        } else if (s_cmdLen < (int)sizeof(s_cmdBuf) - 1) {
            s_cmdBuf[s_cmdLen++] = c;
        }
    }
}
