/**
 * @file gateway_diag.cpp
 * @brief Gateway diagnostics counters and their JSON form (see gateway_diag.h).
 */

#include "gateway_diag.h"
#include <ctype.h>
#include <string.h>
#ifndef HOST_TEST
#include <esp_system.h>
#endif

GatewayDiag g_gatewayDiag = {};

void gatewayDiagNoteRejected(const char* deviceId) {
    if (!deviceId) return;
    for (int i = 0; i < 8; i++) {
        if (!isalnum((unsigned char)deviceId[i])) return;   // also stops at a short ID's '\0'
    }
    memcpy(g_gatewayDiag.lastRejectedId, deviceId, 8);
    g_gatewayDiag.lastRejectedId[8] = '\0';
}

#ifndef HOST_TEST
static const char* resetReasonName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        case ESP_RST_UNKNOWN:   return "unknown";
        default:                return "other";
    }
}

GatewayDiagSample gatewayDiagSample() {
    GatewayDiagSample s;
    s.uptimeS = millis() / 1000;
    s.resetReason = resetReasonName(esp_reset_reason());
    s.freeHeap = ESP.getFreeHeap();
    s.minFreeHeap = ESP.getMinFreeHeap();
    return s;
}
#endif

void gatewayDiagAppendJson(String& out, const GatewayDiagSample& s) {
    const GatewayDiag& d = g_gatewayDiag;
    out += "\"uptime\":" + String((unsigned long)s.uptimeS) + ",";
    out += "\"reset_reason\":\"" + String(s.resetReason ? s.resetReason : "unknown") + "\",";
    out += "\"free_heap\":" + String((unsigned long)s.freeHeap) + ",";
    out += "\"min_free_heap\":" + String((unsigned long)s.minFreeHeap) + ",";
    out += "\"rx_crc_ok\":" + String((unsigned long)d.rxCrcOk) + ",";
    out += "\"auth_ok\":" + String((unsigned long)d.authOk) + ",";
    out += "\"auth_fail\":" + String((unsigned long)d.authFail) + ",";
    out += "\"replay_drop\":" + String((unsigned long)d.replayDrop) + ",";
    out += "\"pic_stale\":" + String((unsigned long)d.picStale) + ",";
    out += "\"session_full\":" + String((unsigned long)d.sessionFull) + ",";
    // only ever an 8-character alphanumeric ID (gatewayDiagNoteRejected), safe unescaped
    if (d.lastRejectedId[0])
        out += "\"last_rejected_id\":\"" + String(d.lastRejectedId) + "\"";
    else
        out += "\"last_rejected_id\":null";
}

void gatewayDiagClear() {
    memset(&g_gatewayDiag, 0, sizeof(g_gatewayDiag));
}
