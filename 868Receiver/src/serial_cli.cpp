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
#include "downlink.h"
#include "picture_token.h"
#include "duty_cycle.h"
#include "crypto.h"
#include "image_receiver.h"
#include "gateway_diag.h"
#include "radio_task.h"
#ifdef RADIO_EVAL
#include "sniff_format.h"
#endif
#include <WiFi.h>
#include "esp_wifi.h"

static char s_cmdBuf[256];
static int s_cmdLen = 0;
static bool s_cmdOverflow = false;

void serialCliInit() {
    s_cmdLen = 0;
    s_cmdOverflow = false;
}

static void printHelp() {
    Serial.println("\n--- VERISURE GATEWAY SERIAL CLI ---");
    Serial.println("  help                       - Show this help menu");
    Serial.println("  status                     - Display gateway and sensor status");
    Serial.println("  testspi [1|2]              - Test CC1101 #1 (default) or #2 SPI bus and registers");
    Serial.println("  scan                       - Scan and list available Wi-Fi networks");
    Serial.println("  connect <ssid> [password]  - Connect to Wi-Fi and save to NVS");
    Serial.println("  setkey <32-hex-characters> - Configure Master AES Key");
    Serial.println("  setmqtt <host> <port> [user] [pass] [topic] - Configure MQTT broker");
    Serial.println("  alarm [on|off]             - Show or set the alarm state sent to cameras");
    Serial.println("  drop <n>                   - Test: drop every n-th camera picture chunk (0 = off)");
    Serial.println("  replay reset <ID> [block]  - Forget a device's replay counter (after an unseeded reflash);");
    Serial.println("                               then only its boot packet of blocks 1..block (default 8) is taken");
    Serial.println("  freqoffset [n]             - Show or set CC1101 #1 frequency correction (FSCTRL0 steps)");
    Serial.println("  freqoffset2 [n]            - Show or set CC1101 #2 frequency correction (FSCTRL0 steps)");
    Serial.println("  pch [on|off|cal|status]    - Show or control picture channel module #2");
    Serial.println("  duty                       - Show 1 h duty-cycle and band-50 fallback counters");
    Serial.println("  portalpass [<password>|clear] - Show, set (8-64 characters) or clear the web portal password");
    Serial.println("  appass                     - Show the setup access point's password (unique per gateway)");
    Serial.println("  radio [reset]              - Show (or clear) the radio task diagnostics");
#ifdef RADIO_EVAL
    Serial.println("  loopblock <ms>             - Eval: block loop() once for <ms> (max 4000, below the loop watchdog)");
    Serial.println("  wdttest                    - Eval: block loop() without feeding the watchdog (reboots, reset reason task_wdt)");
    Serial.println("  radiostall <ms>            - Eval: the radio task skips FIFO servicing once for <ms> (max 3000)");
    Serial.println("  polldelay <ms>             - Eval: delay every camera poll answer by <ms> before it is queued (0 = off)");
    Serial.println("  sniff [on|off|all|unknown] - Eval: dump rejected/bad-CRC frames ('on'/'unknown') or every frame ('all') as [Sniff#n] lines; no argument shows the mode");
#endif
    Serial.println("  reset                      - Factory reset all NVS settings");
    Serial.println("  reboot                     - Restart gateway microcontroller");
    Serial.println("-----------------------------------\n");
}

static void runTestSpi(uint8_t module) {
    const uint8_t cs = (module == 1) ? PIN_CS2 : PIN_CS;
    Serial.printf("\n--- SPI & CC1101 #%u HARDWARE DIAGNOSTICS ---\n", (unsigned)(module + 1));
    Serial.printf("Pins: SCK=GPIO%d, MISO=GPIO%d, MOSI=GPIO%d, CS=GPIO%d\n",
                  PIN_SCK, PIN_MISO, PIN_MOSI, cs);

    // The radio task owns the SPI bus: it runs the pin test and the SRES sequence (radio_task.cpp,
    // radioDoTestSpi) and returns the readings; it also restores the radio registers afterwards.
    RadioTestSpiResult r;
    if (!radioRequestTestSpi(module, &r, 1000)) {
        Serial.println("[-] ERROR: the radio task did not answer (not running or busy).\n");
        return;
    }
    const int miso_high = r.misoHigh, miso_low = r.misoLow;
    const byte sres_st = r.sresSt, st_part = r.partSt, val_part = r.part;
    const byte st_ver = r.verSt, val_ver = r.ver, st_marc = r.marcSt, val_marc = r.marc;

    Serial.printf("CS HIGH (with Pullup) -> MISO: %d (Expected: 1 when CC1101 is deselected)\n", miso_high);
    Serial.printf("CS LOW                -> MISO: %d (MUST be 0 when CC1101 responds!)\n", miso_low);

    Serial.printf("After SRES Reset:\n");
    Serial.printf("  SRES Status: 0x%02X (CHIP_RDYn=%d, STATE=%d)\n", sres_st, (sres_st >> 7) & 1, (sres_st >> 4) & 7);
    Serial.printf("  PARTNUM    : 0x%02X (Status: 0x%02X, expected: 0x00)\n", val_part, st_part);
    Serial.printf("  VERSION    : 0x%02X (Status: 0x%02X, expected: 0x04 or 0x14)\n", val_ver, st_ver);
    Serial.printf("  MARCSTATE  : 0x%02X (Status: 0x%02X, 0x01=IDLE, 0x0D=RX)\n", val_marc, st_marc);

    if (val_ver == 0x04 || val_ver == 0x14) {
        Serial.printf("[+] SPI and CC1101 #%u communication OK!\n", (unsigned)(module + 1));
        Serial.println("[+] Radio registers restored, back in RX.\n");
    } else {
        Serial.printf("[-] ERROR: CC1101 #%u did not respond correctly over SPI.\n", (unsigned)(module + 1));
        Serial.println("[-] Radio offline: the registers are restored once the module answers again.\n");
    }
}

/** Radio task diagnostics (all values are cached/counted by the task; no SPI access here). */
static void printRadioDiag() {
    const RadioDiag& d = g_radioDiag;
    const RadioLoopDiag& l = g_radioLoopDiag;
    Serial.printf("  Radio #1     : %s | version 0x%02X | MARCSTATE 0x%02X | RSSI %d dBm (868.950 MHz)\n",
                  g_radioStatus.online ? "online" : "OFFLINE", g_radioStatus.version,
                  g_radioStatus.marcState, (int)g_radioStatus.rssiDbm);
    Serial.printf("  Radio #1 RX  : frames %lu | FIFO overflows %lu | partial drops %lu | queue drops %lu (high water %lu/%d)\n",
                  (unsigned long)d.frames, (unsigned long)d.fifoOverflows, (unsigned long)d.partialDrops,
                  (unsigned long)d.queueDrops, (unsigned long)d.queueHighWater, RADIO_QUEUE_DEPTH);
    Serial.printf("                 task gap max %lu us | >5 ms %lu | >10 ms %lu | >20 ms %lu | drain gap max %lu ms\n",
                  (unsigned long)d.maxGapUs, (unsigned long)d.gapOver5ms, (unsigned long)d.gapOver10ms,
                  (unsigned long)d.gapOver20ms, (unsigned long)l.maxDrainGapMs);
    Serial.printf("  Radio #1 TX  : done %lu | late %lu | timeouts %lu | aborted RX %lu | max latency %lu us\n",
                  (unsigned long)d.txDone, (unsigned long)d.txLate, (unsigned long)d.txTimeouts,
                  (unsigned long)d.txAbortedRx, (unsigned long)d.maxTxLatencyUs);
    Serial.printf("                 poll answers %lu | skipped late %lu | max answer age %lu us\n",
                  (unsigned long)l.pollAnswers, (unsigned long)l.pollLate, (unsigned long)l.maxPollAnswerAgeUs);
    Serial.printf("  Radio #1 hlth: slow status reads (>5 ms) %lu | unstable %lu | RX restarts %lu | re-inits %lu | stack free %lu B\n",
                  (unsigned long)d.slowStatusReads, (unsigned long)d.statusUnstable, (unsigned long)d.rxRestarts,
                  (unsigned long)d.rxReinits, (unsigned long)d.stackFreeMin);

    const RadioDiag& d2 = g_radioDiag2;
    Serial.printf("  Radio #2     : %s | mode %s | version 0x%02X | MARCSTATE 0x%02X | RSSI %d dBm\n",
                  g_radioStatus2.online ? "online" : "OFFLINE",
                  radioIsCalMode() ? "CAL 868.950 MHz" : "PCH 869.525 MHz",
                  g_radioStatus2.version, g_radioStatus2.marcState, (int)g_radioStatus2.rssiDbm);
    Serial.printf("  Radio #2 RX  : frames %lu | FIFO overflows %lu | partial drops %lu | queue drops %lu | restarts %lu | re-inits %lu\n",
                  (unsigned long)d2.frames, (unsigned long)d2.fifoOverflows, (unsigned long)d2.partialDrops,
                  (unsigned long)d2.queueDrops, (unsigned long)d2.rxRestarts, (unsigned long)d2.rxReinits);
}

static void printPchStatus() {
    const GatewayConfig& cfg = ConfigManager::get();
    PchCalStats cal;
    pchCalGetStats(&cal);
    Serial.println("\n--- PICTURE CHANNEL (CC1101 #2) ---");
    Serial.printf("  PCH Enabled  : %s (NVS)\n", cfg.pchEnabled ? "ON" : "OFF");
    Serial.printf("  Module #2    : %s | mode %s | version 0x%02X | MARCSTATE 0x%02X | RSSI %d dBm\n",
                  g_radioStatus2.online ? "online" : "OFFLINE",
                  radioIsCalMode() ? "CAL (868.950 MHz)" : "PCH (869.525 MHz)",
                  g_radioStatus2.version, g_radioStatus2.marcState, (int)g_radioStatus2.rssiDbm);
    Serial.printf("  freqoffset2  : %d (%+.1f kHz)\n", cfg.freqOffset2, cfg.freqOffset2 * 1.587);
    if (cal.count > 0) {
        float avg = (float)cal.freqEstSum / (float)cal.count;
        Serial.printf("  CAL stats    : %lu matched pair(s) | avg #2 freqEst %+.2f (%+.1f kHz)\n",
                      (unsigned long)cal.count, avg, avg * (26000.0f / 16384.0f));
        Serial.printf("                 last RSSI #1 %d dBm (freqEst %+d) | #2 %d dBm (freqEst %+d)\n",
                      (int)cal.lastRssi1, (int)cal.lastFreqEst1, (int)cal.lastRssi2, (int)cal.lastFreqEst2);
        Serial.printf("                 suggested freqoffset2: %d\n", (int)cal.suggestedOffset);
    } else {
        Serial.println("  CAL stats    : none yet (run 'pch cal' while a device transmits)");
    }
    imageReceiverPchPrintStatus();
    {
        PicTokenStatus tok;
        pictureTokenGetStatus((uint32_t)millis(), &tok);
        const char* stName = (tok.state == PIC_TOK_HELD) ? "held"
                           : (tok.state == PIC_TOK_RESERVED) ? "reserved" : "idle";
        if (tok.state == PIC_TOK_IDLE) {
            Serial.println("  Token        : idle");
        } else {
            Serial.printf("  Token        : %s | holder %s%s | hold %lu ms | lease rem %lu ms\n",
                          stName, tok.holderId, tok.holderLegacy ? " (legacy)" : "",
                          (unsigned long)tok.holdMs, (unsigned long)tok.leaseRemMs);
        }
        Serial.print("  Token queue  : ");
        if (tok.waitCount == 0) {
            Serial.println("empty");
        } else {
            for (uint8_t i = 0; i < tok.waitCount; i++) {
                if (i > 0) Serial.print(", ");
                Serial.print(tok.waitIds[i]);
            }
            Serial.println();
        }
        Serial.printf("  Token stats  : grants %lu | holds %lu | releases %lu | expiries %lu | legacy %lu\n",
                      (unsigned long)tok.grants, (unsigned long)tok.holds,
                      (unsigned long)tok.releases, (unsigned long)tok.expiries,
                      (unsigned long)tok.legacyClaims);
    }
    {
        DutyCycleStatus dc;
        dutyCycleGetStatus((uint32_t)millis(), &dc);
        Serial.printf("  Duty GW TX   : %lu ms (%lu.%lu%% of 3600 ms/h) | peak %lu ms | over %lu\n",
                      (unsigned long)dc.gwTx1hMs,
                      (unsigned long)(dc.gwTxBudgetPctTenths / 10),
                      (unsigned long)(dc.gwTxBudgetPctTenths % 10),
                      (unsigned long)dc.gwTxPeak1hMs,
                      (unsigned long)dc.gwTxOverBudgetCount);
        Serial.printf("  Fallback B50 : %lu pic(s) | %lu chunk(s) | %lu ms | cam B50 over %lu\n",
                      (unsigned long)dc.totalFallbackPictures,
                      (unsigned long)dc.totalFallbackChunks,
                      (unsigned long)dc.totalFallbackAirtimeMs,
                      (unsigned long)dc.totalCamB50OverBudgetCount);
    }
    Serial.println("-----------------------------------\n");
}

static void printDutyStatus() {
    DutyCycleStatus dc;
    dutyCycleGetStatus((uint32_t)millis(), &dc);
    Serial.println("\n--- DUTY CYCLE & FALLBACK (1H SLIDING) ---");
    Serial.printf("  GW TX (B50)  : %lu ms (%lu.%lu%% of 3600 ms/h) | peak %lu ms | over %lu%s | total %lu pkt (%lu ms)\n",
                  (unsigned long)dc.gwTx1hMs,
                  (unsigned long)(dc.gwTxBudgetPctTenths / 10),
                  (unsigned long)(dc.gwTxBudgetPctTenths % 10),
                  (unsigned long)dc.gwTxPeak1hMs,
                  (unsigned long)dc.gwTxOverBudgetCount,
                  dc.gwTxOverBudget ? " [OVER]" : "",
                  (unsigned long)dc.gwTxPackets,
                  (unsigned long)dc.gwTxTotalMs);
    Serial.printf("  GW Wake (B54): %lu ms (%lu.%lu%% of 360000 ms/h)%s | total %lu pkt (%lu ms)\n",
                  (unsigned long)dc.gwB54_1hMs,
                  (unsigned long)(dc.gwB54PctTenths / 10),
                  (unsigned long)(dc.gwB54PctTenths % 10),
                  dc.gwB54OverBudget ? " [OVER]" : "",
                  (unsigned long)dc.gwB54Packets,
                  (unsigned long)dc.gwB54TotalMs);
    Serial.printf("  Fallback B50 : %lu pic(s) | %lu chunk(s) | %lu ms | cam B50 over %lu | bucket age %lu s\n",
                  (unsigned long)dc.totalFallbackPictures,
                  (unsigned long)dc.totalFallbackChunks,
                  (unsigned long)dc.totalFallbackAirtimeMs,
                  (unsigned long)dc.totalCamB50OverBudgetCount,
                  (unsigned long)(dc.curBucketAgeMs / 1000UL));
    if (dc.cameraCount == 0) {
        Serial.println("  Cameras      : none seen yet");
    } else {
        for (uint8_t i = 0; i < dc.cameraCount; i++) {
            const CamDutyStatus& c = dc.cameras[i];
            Serial.printf("  Camera %-8s: B50 %lu ms (%lu.%lu%%, peak %lu, over %lu%s) | B54 %lu ms (%lu.%lu%%, peak %lu, over %lu%s)\n",
                          c.deviceId,
                          (unsigned long)c.b50_1hMs,
                          (unsigned long)(c.b50BudgetPctTenths / 10),
                          (unsigned long)(c.b50BudgetPctTenths % 10),
                          (unsigned long)c.b50Peak1hMs,
                          (unsigned long)c.b50OverBudgetCount,
                          c.b50OverBudget ? " OVER" : "",
                          (unsigned long)c.b54_1hMs,
                          (unsigned long)(c.b54BudgetPctTenths / 10),
                          (unsigned long)(c.b54BudgetPctTenths % 10),
                          (unsigned long)c.b54Peak1hMs,
                          (unsigned long)c.b54OverBudgetCount,
                          c.b54OverBudget ? " OVER" : "");
            Serial.printf("                 pkt24 %lu | PCH chunks %lu | fallback %lu pic (%lu ch, %lu ms)\n",
                          (unsigned long)c.pkt24Count,
                          (unsigned long)c.pchChunks,
                          (unsigned long)c.fallbackPictures,
                          (unsigned long)c.fallbackChunks,
                          (unsigned long)c.fallbackAirtimeMs);
        }
    }
    Serial.println("------------------------------------------\n");
}

static void printStatus() {
    const GatewayConfig& cfg = ConfigManager::get();
    Serial.println("\n================ GATEWAY STATUS ================");
    Serial.printf("  Uptime       : %lu seconds\n", millis() / 1000);
    Serial.printf("  Wi-Fi Mode   : %s\n", webPortalIsApMode() ? "Access Point (AP)" : "Station (STA)");
    Serial.printf("  IP Address   : %s\n", webPortalGetIp().c_str());
    Serial.printf("  Wi-Fi SSID   : %s (Signal: %d dBm)\n",
                  webPortalIsApMode() ? "Verisure-Gateway" : cfg.wifiSsid,
                  webPortalIsApMode() ? 0 : WiFi.RSSI());
    Serial.printf("  MQTT Status  : %s\n", mqttGetStatusString().c_str());
    char kcv[7];
    cryptoKeyCheckValue(cfg.masterKey, kcv);
    Serial.printf("  Master Key   : set (check value %s)\n", kcv);   // never print the key
    Serial.printf("  Freq. offset : %d / %d (FSCTRL0, x 1.587 kHz) | PCH: %s\n",
                  cfg.freqOffset, cfg.freqOffset2, cfg.pchEnabled ? "on" : "off");
    const GatewayDiagSample ds = gatewayDiagSample();
    const GatewayDiag& d = g_gatewayDiag;
    Serial.printf("  Last reset   : %s\n", ds.resetReason);
    Serial.printf("  Free heap    : %lu B (minimum %lu B)\n", (unsigned long)ds.freeHeap,
                  (unsigned long)ds.minFreeHeap);
    Serial.printf("  RX (boot)    : CRC OK %lu | auth OK %lu | auth rejected %lu | replay dropped %lu\n",
                  (unsigned long)d.rxCrcOk, (unsigned long)d.authOk, (unsigned long)d.authFail,
                  (unsigned long)d.replayDrop);
    Serial.printf("                 stale picture chunks %lu | session table full %lu | last rejected %s\n",
                  (unsigned long)d.picStale, (unsigned long)d.sessionFull,
                  d.lastRejectedId[0] ? d.lastRejectedId : "-");
    printRadioDiag();
    Serial.printf("  Sensors      : %d registered\n", sensorRegistryGetCount());

    int count = sensorRegistryGetCount();
    for (int i = 0; i < count; i++) {
        const SensorRecord* rec = sensorRegistryGetByIndex(i);
        if (!rec) continue;
        Serial.printf("    - ID: %s | Contact: %s | Shock: %s | Tamper: %s | Batt: %u mV (%u%%) | RSSI: %d dBm | Packets: %u\n",
                      rec->lastTelem.deviceId,
                      rec->lastTelem.isOpen ? "OPEN [ALARM]" : "CLOSED",
                      rec->lastTelem.isShock ? "SHOCK!" : "Normal",
                      rec->lastTelem.isButton ? "TAMPER!" : "Secured",
                      rec->lastTelem.batteryMv, rec->lastTelem.batteryPct,
                      rec->lastTelem.rssi, (unsigned)rec->packetCount);
    }
    Serial.println("================================================\n");
}

static void runScan() {
    Serial.println("[Wi-Fi] Scanning for networks...");
    const bool ap = webPortalIsApMode();
    if (ap) {
        WiFi.mode(WIFI_AP_STA);   // keep the setup access point up while scanning
    } else {
        WiFi.disconnect(true);
        delay(200);
        WiFi.mode(WIFI_STA);
    }
    delay(100);
    // Async scan polled here: the synchronous call can take longer than the 5 s loop watchdog
    WiFi.scanNetworks(true, true);
    const unsigned long scanStart = millis();
    int n;
    while ((n = WiFi.scanComplete()) == WIFI_SCAN_RUNNING && millis() - scanStart < 15000) {
        feedLoopWDT();
        delay(50);
    }
    if (n < 0) {
        Serial.println(n == WIFI_SCAN_RUNNING ? "[Wi-Fi] Scan still running after 15 s, giving up."
                                              : "[Wi-Fi] Scan failed.");
        return;
    }
    Serial.printf("[Wi-Fi] Found %d networks:\n", n);
    for (int i = 0; i < n; i++) {
        wifi_auth_mode_t auth = WiFi.encryptionType(i);
        const char* authStr = (auth == WIFI_AUTH_OPEN) ? "Open" :
                              (auth == WIFI_AUTH_WPA_PSK) ? "WPA" :
                              (auth == WIFI_AUTH_WPA2_PSK) ? "WPA2" :
                              (auth == WIFI_AUTH_WPA_WPA2_PSK) ? "WPA/WPA2" :
                              (auth == WIFI_AUTH_WPA2_ENTERPRISE) ? "Enterprise" :
                              (auth == WIFI_AUTH_WPA3_PSK) ? "WPA3" :
                              (auth == WIFI_AUTH_WPA2_WPA3_PSK) ? "WPA2/WPA3" : "Encrypted";
        Serial.printf("  %2d: %-20s (Ch %2d, %3d dBm) [%s] BSSID: %s\n",
                      i + 1,
                      WiFi.SSID(i).c_str(),
                      (int)WiFi.channel(i),
                      (int)WiFi.RSSI(i),
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
    } else if (strcmp(cmd, "testspi") == 0 || strncmp(cmd, "testspi ", 8) == 0) {
        if (cmd[7] == '\0') {
            runTestSpi(0);
        } else {
            const char* arg = cmd + 8;
            while (*arg == ' ') arg++;
            if (strcmp(arg, "1") == 0) runTestSpi(0);
            else if (strcmp(arg, "2") == 0) runTestSpi(1);
            else Serial.println("[CLI] Usage: testspi [1|2]");
        }
    } else if (strcmp(cmd, "radio") == 0) {
        Serial.println();
        printRadioDiag();
        Serial.println();
    } else if (strcmp(cmd, "radio reset") == 0) {
        radioDiagReset();
        Serial.println("[CLI] Radio diagnostics cleared.");
    } else if (strcmp(cmd, "duty") == 0) {
        printDutyStatus();
#ifdef RADIO_EVAL
    } else if (strncmp(cmd, "loopblock ", 10) == 0) {
        unsigned long ms = strtoul(cmd + 10, nullptr, 10);
        if (ms > 4000) ms = 4000;   // below the 5 s loop watchdog: wdttest triggers it on purpose
        Serial.printf("[CLI] Blocking loop() for %lu ms (max 4000)\n", ms);
        Serial.flush();
        delay(ms);
    } else if (strcmp(cmd, "wdttest") == 0) {
        Serial.println("[CLI] Blocking loop() without feeding the watchdog: expect a reboot after ~5 s (reset reason task_wdt)");
        Serial.flush();
        delay(15000);
        Serial.println("[CLI] ERROR: the loop watchdog did NOT reset the gateway");
    } else if (strncmp(cmd, "radiostall ", 11) == 0) {
        unsigned long ms = strtoul(cmd + 11, nullptr, 10);
        if (ms > 3000) ms = 3000;
        radioEvalStall((uint32_t)ms);
        Serial.printf("[CLI] Radio task skips FIFO servicing once for %lu ms (max 3000)\n", ms);
    } else if (strncmp(cmd, "polldelay ", 10) == 0) {
        unsigned long ms = strtoul(cmd + 10, nullptr, 10);
        if (ms > 500) ms = 500;
        g_radioEvalPollDelayMs = (uint32_t)ms;
        Serial.printf("[CLI] Poll answers delayed by %lu ms (max 500)%s\n", ms, ms ? "" : " (off)");
    } else if (strcmp(cmd, "sniff") == 0 || strncmp(cmd, "sniff ", 6) == 0) {
        const char* arg = cmd + 5;
        while (*arg == ' ') arg++;
        bool ok = true;
        if (*arg == '\0') {
            // plain "sniff": show the mode below
        } else if (strcmp(arg, "on") == 0 || strcmp(arg, "unknown") == 0) {
            g_sniffMode = SNIFF_UNKNOWN;
        } else if (strcmp(arg, "all") == 0) {
            g_sniffMode = SNIFF_ALL;
        } else if (strcmp(arg, "off") == 0) {
            g_sniffMode = SNIFF_OFF;
        } else {
            ok = false;
            Serial.println("[CLI] Usage: sniff [on|off|all|unknown]");
        }
        if (ok) {
            const uint8_t m = g_sniffMode;
            Serial.printf("[CLI] Sniff mode: %s\n", m == SNIFF_ALL ? "all (every frame)" :
                          m == SNIFF_UNKNOWN ? "unknown (rejected or bad-CRC frames)" : "off");
        }
#endif
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

        if (ssid[0] == '\0') {
            Serial.println("[CLI] Usage: connect <ssid> [password]");
            return;
        }

        Serial.printf("[CLI] Connecting to SSID \"%s\"...\n", ssid);
        const bool ap = webPortalIsApMode() || ((WiFi.getMode() & WIFI_AP) != 0);
        if (ap) {
            WiFi.mode(WIFI_AP_STA);   // keep the setup access point alive during the attempt
            delay(100);
        } else {
            WiFi.disconnect(true, true);
            delay(200);
            WiFi.mode(WIFI_STA);
        }
        WiFi.begin(ssid, pass);

        unsigned long t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
            feedLoopWDT();   // waits up to 10 s, longer than the loop watchdog
            delay(250);
            Serial.print(".");
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[CLI] Connected! IP: %s | RSSI: %d dBm\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
            ConfigManager::setWifi(ssid, pass);
            ConfigManager::save();
            webPortalOnStaConnected();
        } else {
            Serial.println("[CLI] Connection failed.");
            if (ap) {
                WiFi.disconnect(false);
                WiFi.mode(WIFI_AP);
            }
        }
    } else if (strncmp(cmd, "setkey ", 7) == 0) {
        char* keyStr = cmd + 7;
        while (*keyStr == ' ') keyStr++;
        if (strlen(keyStr) == 32 && ConfigManager::setMasterKeyHex(keyStr)) {
            ConfigManager::save();
            char kcv[7];
            cryptoKeyCheckValue(ConfigManager::get().masterKey, kcv);
            Serial.printf("[CLI] Master Key saved (check value %s)\n", kcv);
        } else {
            Serial.println("[CLI] Error: Key must be exactly 32 hex characters!");
        }
    } else if (strncmp(cmd, "setmqtt ", 8) == 0) {
        char host[65] = {0};
        int port = 1883;
        char user[33] = {0};
        char pass[65] = {0};
        char topic[33] = "verisure";
        int parsed = sscanf(cmd + 8, "%64s %d %32s %64s %32s", host, &port, user, pass, topic);
        if (parsed >= 2) {
            if (ConfigManager::setMqtt(host, (uint16_t)port, user, pass, topic)) {
                ConfigManager::save();
                Serial.printf("[CLI] MQTT saved: %s:%d (Topic: %s)\n", host, port, ConfigManager::get().mqttTopic);
            }
        } else {
            Serial.println("[CLI] Usage: setmqtt <host> <port> [user] [pass] [topic]");
        }
    } else if (strcmp(cmd, "alarm on") == 0 || strcmp(cmd, "alarm off") == 0) {
        downlinkSetArmed(cmd[7] == 'n');
        mqttPublishAlarmState();   // keep the HA switch in step
        Serial.printf("[CLI] Alarm %s (sent to cameras with their next poll)\n", downlinkArmed() ? "ARMED" : "DISARMED");
    } else if (strncmp(cmd, "drop ", 5) == 0) {
        unsigned long n = strtoul(cmd + 5, nullptr, 10);
        imageReceiverSetDrop((uint32_t)n);
        Serial.printf("[CLI] Dropping every %lu-th picture chunk%s\n", n, n ? "" : " (off)");
    } else if (strncmp(cmd, "replay reset ", 13) == 0) {
        char id[9] = {0};
        unsigned long block = 0;
        int n = sscanf(cmd + 13, "%8s %lu", id, &block);
        if (n < 1 || strlen(id) != 8) {
            Serial.println("[CLI] Usage: replay reset <ID> [max block, default 8]");
        } else {
            bool had = cryptoReplayReset(id, (uint32_t)block);
            pictureTokenRelease(id, (uint32_t)millis());
            Serial.printf("[CLI] Replay state for \"%s\" %s; waiting for a boot packet of blocks 1..%lu "
                          "(power-cycle the device if it has booted already)\n",
                          id, had ? "cleared" : "not found", block ? block : 8UL);
        }
    } else if (strcmp(cmd, "alarm") == 0) {
        Serial.printf("[CLI] Alarm is %s\n", downlinkArmed() ? "ARMED" : "DISARMED");
    } else if (strcmp(cmd, "freqoffset") == 0 || strncmp(cmd, "freqoffset ", 11) == 0) {
        if (cmd[10] == '\0') {
            Serial.printf("[CLI] Frequency offset #1: %d (x 1.587 kHz)\n", ConfigManager::get().freqOffset);
        } else {
            char* end;
            long n = strtol(cmd + 11, &end, 10);
            if (end == cmd + 11 || *end != '\0' || n < -128 || n > 127) {
                Serial.println("[CLI] Error: freqoffset takes a number from -128 to 127.");
            } else {
                ConfigManager::setFreqOffset((int8_t)n);
                ConfigManager::save();
                // the radio task writes FSCTRL0 (it owns the SPI bus); the new value is in RAM already
                const bool wasOnline = g_radioStatus.online;
                const bool applied = radioRequestReinit(0, 200);
                const char* note = !applied ? " (radio #1 not re-initialized: task busy or not running)"
                                 : !wasOnline ? " (radio #1 offline: used when the module is detected)" : "";
                Serial.printf("[CLI] Frequency offset #1 set to %ld (%+.1f kHz) and saved%s.\n", n, n * 1.587, note);
            }
        }
    } else if (strcmp(cmd, "freqoffset2") == 0 || strncmp(cmd, "freqoffset2 ", 12) == 0) {
        if (cmd[11] == '\0') {
            Serial.printf("[CLI] Frequency offset #2: %d (x 1.587 kHz)\n", ConfigManager::get().freqOffset2);
        } else {
            char* end;
            long n = strtol(cmd + 12, &end, 10);
            if (end == cmd + 12 || *end != '\0' || n < -128 || n > 127) {
                Serial.println("[CLI] Error: freqoffset2 takes a number from -128 to 127.");
            } else {
                ConfigManager::setFreqOffset2((int8_t)n);
                ConfigManager::save();
                pchCalReset();
                const bool wasOnline = g_radioStatus2.online;
                const bool applied = radioRequestReinit(1, 200);
                const char* note = !applied ? " (radio #2 not re-initialized: task busy or not running)"
                                 : !wasOnline ? " (radio #2 offline: used when the module is detected)" : "";
                Serial.printf("[CLI] Frequency offset #2 set to %ld (%+.1f kHz) and saved%s.\n", n, n * 1.587, note);
            }
        }
    } else if (strcmp(cmd, "pch") == 0 || strncmp(cmd, "pch ", 4) == 0) {
        const char* arg = (cmd[3] == '\0') ? "status" : cmd + 4;
        while (*arg == ' ') arg++;
        if (*arg == '\0' || strcmp(arg, "status") == 0) {
            printPchStatus();
        } else if (strcmp(arg, "on") == 0 || strcmp(arg, "off") == 0) {
            const bool en = (strcmp(arg, "on") == 0);
            ConfigManager::setPchEnabled(en);
            ConfigManager::save();
            if (en) imageReceiverPchReset();
            const bool applied = radioRequestSetMode(false, 200);
            Serial.printf("[CLI] Picture channel %s and saved (module #2 mode: PCH 869.525 MHz%s%s).\n",
                          en ? "ENABLED" : "DISABLED",
                          applied ? "" : ", task busy",
                          g_radioStatus2.online ? "" : ", module #2 offline");
        } else if (strcmp(arg, "cal") == 0) {
            pchCalReset();
            const bool applied = radioRequestSetMode(true, 200);
            Serial.printf("[CLI] Picture channel calibration mode ON (module #2 on 868.950 MHz%s%s).\n",
                          applied ? "" : ", task busy",
                          g_radioStatus2.online ? "" : ", module #2 offline");
            Serial.println("[CLI] Run 'pch status' to inspect matches, 'pch on'/'pch off' to exit CAL mode.");
        } else {
            Serial.println("[CLI] Usage: pch [on|off|cal|status]");
        }
    } else if (strcmp(cmd, "appass") == 0) {
        char apPass[64];
        ConfigManager::apPassword(apPass);
        Serial.printf("[CLI] Setup access point 'Verisure-Gateway' password: %s\n", apPass);
        memset(apPass, 0, sizeof(apPass));
    } else if (strcmp(cmd, "portalpass") == 0) {
        Serial.printf("[CLI] Web portal password: %s\n", ConfigManager::hasPortalPassword() ? "set" : "NOT SET");
    } else if (strcmp(cmd, "portalpass clear") == 0) {
        ConfigManager::clearPortalPassword();
        Serial.println("[CLI] Web portal password cleared. On Wi-Fi the web page is locked until a new one is set; in the setup access point it is open; DEFAULT_PORTAL_PASS in secrets.h sets it again at the next boot.");
    } else if (strncmp(cmd, "portalpass ", 11) == 0) {
        // the password is never echoed
        if (ConfigManager::setPortalPassword(cmd + 11))
            Serial.println("[CLI] Web portal password set (the browser asks for it; any user name).");
        else
            Serial.println("[CLI] Error: the password must be 8-64 characters.");
    } else if (strcmp(cmd, "reset") == 0) {
        ConfigManager::resetToDefaults();
        Serial.println("[CLI] Reset all settings to factory defaults.");
    } else if (strcmp(cmd, "reboot") == 0) {
        Serial.println("[CLI] Rebooting gateway now...");
        delay(100);
        ESP.restart();
    } else {
        Serial.printf("[CLI] Unknown command: \"%s\". Type 'help' for command list.\n", cmd);
    }
}

void serialCliLoop() {
    // at most 256 bytes per call: a host that streams without a pause must not keep loop() in here
    for (int n = 0; n < 256 && Serial.available(); n++) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            if (s_cmdOverflow) {
                Serial.println("[CLI] Error: command too long");
            } else {
                s_cmdBuf[s_cmdLen] = '\0';
                processCommand(s_cmdBuf);
            }
            feedLoopWDT();   // a command can run for seconds (scan, connect, setkey...): a fresh window for what follows
            s_cmdLen = 0;
            s_cmdOverflow = false;
        } else if (s_cmdOverflow) {
            // drop remaining bytes until end of line
        } else if (s_cmdLen < (int)sizeof(s_cmdBuf) - 1) {
            s_cmdBuf[s_cmdLen++] = c;
        } else {
            s_cmdOverflow = true;
        }
    }
}
