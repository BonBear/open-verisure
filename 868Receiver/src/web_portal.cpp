#include "web_portal.h"
#include "image_receiver.h"
#include "config_manager.h"
#include "sensor_registry.h"
#include "hardware_pins.h"
#include "mqtt_manager.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <SPI.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include "esp_wifi.h"
#include <WiFiMulti.h>

static WebServer s_server(80);
static DNSServer s_dnsServer;
static WiFiMulti s_wifiMulti;
static bool s_isApMode = false;
static unsigned long s_rebootAt = 0;
static int s_cachedRssi = -100;
static uint8_t s_cachedMarc = 0;
static uint32_t s_cachedPackets = 0;

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Verisure Sub-1GHz Gateway</title>
<style>
:root {
  --bg: #0f172a;
  --card: #1e293b;
  --border: #334155;
  --text: #f8fafc;
  --text-muted: #94a3b8;
  --primary: #3b82f6;
  --primary-hover: #2563eb;
  --success: #10b981;
  --danger: #ef4444;
  --warning: #f59e0b;
}
* { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
body { background: var(--bg); color: var(--text); padding: 16px; min-height: 100vh; }
.container { max-width: 900px; margin: 0 auto; display: flex; flex-direction: column; gap: 20px; }
header { display: flex; justify-content: space-between; align-items: center; padding: 16px 20px; background: var(--card); border-radius: 12px; border: 1px solid var(--border); flex-wrap: wrap; gap: 10px; }
.logo-title { display: flex; align-items: center; gap: 12px; }
.shield { font-size: 28px; }
h1 { font-size: 20px; font-weight: 700; }
.badge { display: inline-flex; align-items: center; gap: 6px; padding: 4px 10px; border-radius: 20px; font-size: 12px; font-weight: 600; }
.badge-sta { background: rgba(16, 185, 129, 0.2); color: var(--success); border: 1px solid var(--success); }
.badge-ap { background: rgba(245, 158, 11, 0.2); color: var(--warning); border: 1px solid var(--warning); }
.badge-alarm { background: rgba(239, 68, 68, 0.2); color: var(--danger); border: 1px solid var(--danger); }
.badge-ok { background: rgba(16, 185, 129, 0.2); color: var(--success); border: 1px solid var(--success); }
.card { background: var(--card); border: 1px solid var(--border); border-radius: 12px; padding: 20px; }
.card h2 { font-size: 16px; margin-bottom: 16px; display: flex; align-items: center; gap: 8px; color: #cbd5e1; }
.stats-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 12px; margin-bottom: 16px; }
.stat-box { background: rgba(15, 23, 42, 0.6); padding: 12px; border-radius: 8px; border: 1px solid var(--border); }
.stat-label { font-size: 11px; color: var(--text-muted); text-transform: uppercase; letter-spacing: 0.5px; }
.stat-val { font-size: 18px; font-weight: 700; margin-top: 4px; }
table { width: 100%; border-collapse: collapse; margin-top: 10px; font-size: 14px; }
th, td { padding: 10px 12px; text-align: left; border-bottom: 1px solid var(--border); }
th { color: var(--text-muted); font-size: 12px; font-weight: 600; text-transform: uppercase; }
tr:last-child td { border-bottom: none; }
.form-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }
@media (max-width: 600px) { .form-grid { grid-template-columns: 1fr; } }
.form-group { display: flex; flex-direction: column; gap: 6px; }
.form-group.full { grid-column: 1 / -1; }
label { font-size: 13px; color: var(--text-muted); font-weight: 500; }
input, select { background: #0f172a; border: 1px solid var(--border); color: var(--text); padding: 10px 12px; border-radius: 6px; font-size: 14px; outline: none; }
input:focus { border-color: var(--primary); }
.input-row { display: flex; gap: 8px; }
.input-row input { flex: 1; }
button { background: var(--primary); color: white; border: none; padding: 10px 16px; border-radius: 6px; font-size: 14px; font-weight: 600; cursor: pointer; transition: background 0.2s; }
button:hover { background: var(--primary-hover); }
button.secondary { background: #334155; }
button.secondary:hover { background: #475569; }
button.danger { background: #dc2626; }
button.danger:hover { background: #b91c1c; }
.btn-bar { display: flex; justify-content: flex-end; gap: 12px; margin-top: 20px; flex-wrap: wrap; }
.scan-list { max-height: 150px; overflow-y: auto; background: #0f172a; border: 1px solid var(--border); border-radius: 6px; margin-top: 8px; display: none; }
.scan-item { padding: 8px 12px; cursor: pointer; display: flex; justify-content: space-between; font-size: 13px; border-bottom: 1px solid #1e293b; }
.scan-item:hover { background: #1e293b; }
.pulse-dot { width: 8px; height: 8px; border-radius: 50%; background: var(--success); display: inline-block; animation: pulse 1.5s infinite; }
@keyframes pulse { 0% { opacity: 0.3; transform: scale(0.9); } 50% { opacity: 1; transform: scale(1.2); } 100% { opacity: 0.3; transform: scale(0.9); } }
.toast { position: fixed; bottom: 20px; right: 20px; background: var(--card); border: 1px solid var(--success); color: var(--text); padding: 14px 20px; border-radius: 8px; box-shadow: 0 10px 25px rgba(0,0,0,0.5); display: none; z-index: 100; font-size: 14px; }
</style>
</head>
<body>
<div class="container">
  <header>
    <div class="logo-title">
      <div class="shield">&#128737;&#65039;</div>
      <div>
        <h1>Verisure Sub-1GHz Gateway</h1>
        <div style="font-size: 12px; color: var(--text-muted);">TI CC1101 &bull; 868.950 MHz &bull; AES-128 KDF</div>
      </div>
    </div>
    <div id="net-badge" class="badge badge-sta">Connected</div>
  </header>

  <!-- Live Telemetry -->
  <div class="card">
    <h2><span class="pulse-dot"></span> Live Telemetry & Radio</h2>
    <div class="stats-grid">
      <div class="stat-box">
        <div class="stat-label">Radio Frequency</div>
        <div class="stat-val">868.950 MHz</div>
      </div>
      <div class="stat-box">
        <div class="stat-label">Radio RSSI (Noise Floor)</div>
        <div id="radio-rssi" class="stat-val">- dBm</div>
      </div>
      <div class="stat-box">
        <div class="stat-label">Total Received Packets</div>
        <div id="radio-packets" class="stat-val">0</div>
      </div>
      <div class="stat-box">
        <div class="stat-label">Active Sensors</div>
        <div id="sensor-count" class="stat-val">0</div>
      </div>
    </div>

    <div style="overflow-x: auto;">
      <table>
        <thead>
          <tr>
            <th>Device ID</th>
            <th>Magnet</th>
            <th>Shock</th>
            <th>Tamper</th>
            <th>Battery</th>
            <th>Signal</th>
            <th>Sequence</th>
            <th>Last Seen</th>
          </tr>
        </thead>
        <tbody id="sensors-table-body">
          <tr><td colspan="8" style="text-align: center; color: var(--text-muted); padding: 20px;">Waiting for radio signal from sensors...</td></tr>
        </tbody>
      </table>
    </div>
  </div>

  <!-- Gateway Configuration -->
  <form id="config-form" class="card" onsubmit="saveConfig(event)">
    <h2>&#9881;&#65039; Gateway Configuration</h2>
    <div class="form-grid">
      <div class="form-group full">
        <label>Wi-Fi Network (SSID)</label>
        <div class="input-row">
          <input type="text" id="wifi_ssid" name="wifi_ssid" placeholder="Your Wi-Fi SSID" required>
          <button type="button" class="secondary" onclick="scanWifi()">Scan Wi-Fi</button>
        </div>
        <div id="scan-results" class="scan-list"></div>
      </div>

      <div class="form-group full">
        <label>Wi-Fi Password</label>
        <div class="input-row">
          <input type="password" id="wifi_pass" name="wifi_pass" placeholder="Leave blank if open network">
          <button type="button" class="secondary" onclick="togglePass('wifi_pass')">&#128065;</button>
        </div>
      </div>

      <div class="form-group">
        <label>MQTT Broker Host / IP</label>
        <input type="text" id="mqtt_host" name="mqtt_host" placeholder="e.g. 192.168.0.100">
      </div>

      <div class="form-group">
        <label>MQTT Port</label>
        <input type="number" id="mqtt_port" name="mqtt_port" value="1883">
      </div>

      <div class="form-group">
        <label>MQTT Username</label>
        <input type="text" id="mqtt_user" name="mqtt_user" placeholder="Optional">
      </div>

      <div class="form-group">
        <label>MQTT Password</label>
        <div class="input-row">
          <input type="password" id="mqtt_pass" name="mqtt_pass" placeholder="Optional">
          <button type="button" class="secondary" onclick="togglePass('mqtt_pass')">&#128065;</button>
        </div>
      </div>

      <div class="form-group">
        <label>MQTT Topic Prefix</label>
        <input type="text" id="mqtt_topic" name="mqtt_topic" value="verisure">
      </div>

      <div class="form-group full">
        <label>Master AES-128 Encryption Key (32 hex characters)</label>
        <div class="input-row">
          <input type="text" id="master_key" name="master_key" pattern="[0-9a-fA-F]{32}" maxlength="32" placeholder="Leave blank to keep the current key" autocomplete="off">
          <button type="button" class="secondary" onclick="generateKey()">Generate Key</button>
        </div>
        <small style="color: var(--text-muted); font-size: 11px; margin-top: 4px;">
          Unique device keys are automatically derived from this Master Key via KDF: <code>AES_Master(Device_ID)</code>.
        </small>
      </div>
    </div>

    <div class="btn-bar">
      <button type="button" class="danger" onclick="factoryReset()">Factory Reset NVS</button>
      <button type="button" class="secondary" onclick="rebootGateway()">Reboot</button>
      <button type="submit">Save & Reboot</button>
    </div>
  </form>
</div>

<div id="toast" class="toast"></div>

<script>
function showToast(msg, isErr) {
  const t = document.getElementById('toast');
  t.innerText = msg;
  t.style.borderColor = isErr ? 'var(--danger)' : 'var(--success)';
  t.style.display = 'block';
  setTimeout(() => { t.style.display = 'none'; }, 4000);
}

function togglePass(id) {
  const el = document.getElementById(id);
  el.type = el.type === 'password' ? 'text' : 'password';
}

function generateKey() {
  const bytes = new Uint8Array(16);
  crypto.getRandomValues(bytes);
  const hex = Array.from(bytes).map(b => b.toString(16).padStart(2, '0')).join('').toUpperCase();
  document.getElementById('master_key').value = hex;
}

let initialLoaded = false;
async function fetchStatus() {
  try {
    const res = await fetch('/api/status');
    if (!res.ok) return;
    const data = await res.json();

    // Badges & Stats
    const nb = document.getElementById('net-badge');
    if (data.wifi.mode === 'AP') {
      nb.className = 'badge badge-ap';
      nb.innerText = 'Access Point (' + data.wifi.ip + ')';
    } else {
      nb.className = 'badge badge-sta';
      nb.innerText = 'Wi-Fi: ' + data.wifi.ssid + ' (' + data.wifi.ip + ')';
    }

    document.getElementById('radio-rssi').innerText = data.radio.rssi + ' dBm';
    document.getElementById('radio-packets').innerText = data.radio.total_packets;
    document.getElementById('sensor-count').innerText = data.sensors.length;

    // Prefill form on initial load
    if (!initialLoaded && data.config) {
      document.getElementById('wifi_ssid').value = data.config.wifi_ssid || '';
      document.getElementById('mqtt_host').value = data.config.mqtt_host || '';
      document.getElementById('mqtt_port').value = data.config.mqtt_port || 1883;
      document.getElementById('mqtt_user').value = data.config.mqtt_user || '';
      document.getElementById('mqtt_topic').value = data.config.mqtt_topic || 'verisure';
      document.getElementById('master_key').placeholder = data.config.master_key_set
        ? 'Key is set (hidden). Leave blank to keep it.'
        : '32 hex characters';
      initialLoaded = true;
    }

    // Render sensor table
    const tbody = document.getElementById('sensors-table-body');
    if (data.sensors.length === 0) {
      tbody.innerHTML = '<tr><td colspan="8" style="text-align: center; color: var(--text-muted); padding: 20px;">Waiting for radio signal from sensors...</td></tr>';
    } else {
      let rows = '';
      data.sensors.forEach(s => {
        const magBadge = s.magnet ? '<span class="badge badge-alarm">OPEN (ALARM)</span>' : '<span class="badge badge-ok">CLOSED</span>';
        const shockBadge = s.shock ? '<span class="badge badge-alarm">SHOCK!</span>' : '<span class="badge badge-ok">Clear</span>';
        const btnBadge = s.button ? '<span class="badge badge-alarm">TAMPER</span>' : (s.power_on ? '<span class="badge badge-ap">Power-On</span>' : 'Normal');
        const battBadge = s.low_battery ? '<span style="color:var(--danger)">' + s.battery_mv + ' mV (' + s.battery_pct + '%)</span>' : s.battery_mv + ' mV (' + s.battery_pct + '%)';
        rows += `<tr>
          <td><strong>${s.id}</strong></td>
          <td>${magBadge}</td>
          <td>${shockBadge}</td>
          <td>${btnBadge}</td>
          <td>${battBadge}</td>
          <td>${s.rssi} dBm (LQI ${s.lqi})</td>
          <td>#${s.seq}</td>
          <td>${s.last_seen_sec}s ago</td>
        </tr>`;
      });
      tbody.innerHTML = rows;
    }
  } catch (err) {
    console.error(err);
  }
}

async function scanWifi() {
  const box = document.getElementById('scan-results');
  box.style.display = 'block';
  box.innerHTML = '<div style="padding:10px; color:var(--text-muted); font-size:13px;">Scanning for networks...</div>';
  try {
    const res = await fetch('/api/scan');
    const list = await res.json();
    if (list.length === 0) {
      box.innerHTML = '<div style="padding:10px; color:var(--text-muted); font-size:13px;">No networks found.</div>';
      return;
    }
    let html = '';
    list.forEach(n => {
      html += `<div class="scan-item" onclick="selectWifi('${n.ssid.replace(/'/g, "\\'")}')">
        <span><strong>${n.ssid}</strong> ${n.secure ? '&#128274;' : ''}</span>
        <span style="color:var(--text-muted);">${n.rssi} dBm</span>
      </div>`;
    });
    box.innerHTML = html;
  } catch (err) {
    box.innerHTML = '<div style="padding:10px; color:var(--danger); font-size:13px;">Scan failed.</div>';
  }
}

function selectWifi(ssid) {
  document.getElementById('wifi_ssid').value = ssid;
  document.getElementById('scan-results').style.display = 'none';
  document.getElementById('wifi_pass').focus();
}

async function saveConfig(e) {
  e.preventDefault();
  const form = document.getElementById('config-form');
  const fd = new FormData(form);
  const json = {};
  fd.forEach((v, k) => { json[k] = v; });

  try {
    const res = await fetch('/api/save', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(json)
    });
    const reply = await res.json();
    if (reply.ok) {
      showToast('Settings saved! Rebooting gateway in 3 seconds...', false);
      setTimeout(() => { location.reload(); }, 6000);
    } else {
      showToast('Error: ' + (reply.error || 'Failed to save'), true);
    }
  } catch (err) {
    showToast('Failed to connect to gateway.', true);
  }
}

async function rebootGateway() {
  if (!confirm('Reboot the gateway?')) return;
  try {
    await fetch('/api/reboot', { method: 'POST' });
    showToast('Rebooting gateway... Reloading page in 5 seconds.', false);
    setTimeout(() => { location.reload(); }, 6000);
  } catch (err) {}
}

async function factoryReset() {
  if (!confirm('WARNING: This will erase all stored settings (Wi-Fi, master key, MQTT) in NVS! Continue?')) return;
  try {
    await fetch('/api/reset', { method: 'POST' });
    showToast('NVS erased. Rebooting gateway...', false);
    setTimeout(() => { location.reload(); }, 6000);
  } catch (err) {}
}

setInterval(fetchStatus, 2000);
fetchStatus();
</script>
</body>
</html>
)rawliteral";

// Captive Portal helper
static bool isCaptivePortalRequest() {
    String host = s_server.hostHeader();
    if (host.length() == 0) return false;
    if (s_isApMode && host != "192.168.4.1" && host != "verisure-gateway.local") {
        return true;
    }
    return false;
}

static void handleRoot() {
    if (isCaptivePortalRequest()) {
        s_server.sendHeader("Location", "http://192.168.4.1/", true);
        s_server.send(302, "text/plain", "");
        return;
    }
    s_server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

static void handleApiStatus() {
    const GatewayConfig& cfg = ConfigManager::get();
    unsigned long now = millis();

    String json = "{";
    json += "\"wifi\":{";
    json += "\"mode\":\"" + String(s_isApMode ? "AP" : "STA") + "\",";
    json += "\"connected\":" + String((WiFi.status() == WL_CONNECTED) ? "true" : "false") + ",";
    json += "\"ssid\":\"" + String(s_isApMode ? "Verisure-Gateway" : cfg.wifiSsid) + "\",";
    json += "\"ip\":\"" + (s_isApMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\",";
    json += "\"rssi\":" + String(s_isApMode ? 0 : WiFi.RSSI()) + "},";

    json += "\"radio\":{";
    json += "\"rssi\":" + String(s_cachedRssi) + ",";
    json += "\"marcstate\":\"0x" + String(s_cachedMarc, HEX) + "\",";
    json += "\"total_packets\":" + String(s_cachedPackets) + "},";

    json += "\"mqtt\":{";
    json += "\"connected\":" + String(mqttIsConnected() ? "true" : "false") + ",";
    json += "\"status\":\"" + mqttGetStatusString() + "\"},";

    json += "\"config\":{";
    json += "\"wifi_ssid\":\"" + String(cfg.wifiSsid) + "\",";
    json += "\"mqtt_host\":\"" + String(cfg.mqttHost) + "\",";
    json += "\"mqtt_port\":" + String(cfg.mqttPort) + ",";
    json += "\"mqtt_user\":\"" + String(cfg.mqttUser) + "\",";
    json += "\"mqtt_topic\":\"" + String(cfg.mqttTopic) + "\",";
    // The master key is never sent back to the browser, only whether one is set.
    json += "\"master_key_set\":" + String(cfg.masterKeyHex[0] ? "true" : "false") + "},";

    json += "\"sensors\":[";
    int count = sensorRegistryGetCount();
    for (int i = 0; i < count; i++) {
        const SensorRecord* rec = sensorRegistryGetByIndex(i);
        if (!rec) continue;
        if (i > 0) json += ",";
        unsigned long ageSec = (now >= rec->lastSeenMs) ? (now - rec->lastSeenMs) / 1000 : 0;
        json += "{";
        json += "\"id\":\"" + String(rec->lastTelem.deviceId) + "\",";
        json += "\"seq\":" + String(rec->lastTelem.sequence) + ",";
        json += "\"magnet\":" + String(rec->lastTelem.isOpen ? "true" : "false") + ",";
        json += "\"shock\":" + String(rec->lastTelem.isShock ? "true" : "false") + ",";
        json += "\"button\":" + String(rec->lastTelem.isButton ? "true" : "false") + ",";
        json += "\"power_on\":" + String(rec->lastTelem.isPowerOn ? "true" : "false") + ",";
        json += "\"low_battery\":" + String(rec->lastTelem.isLowBatt ? "true" : "false") + ",";
        json += "\"battery_mv\":" + String(rec->lastTelem.batteryMv) + ",";
        json += "\"battery_pct\":" + String(rec->lastTelem.batteryPct) + ",";
        json += "\"rssi\":" + String(rec->lastTelem.rssi) + ",";
        json += "\"lqi\":" + String(rec->lastTelem.lqi) + ",";
        json += "\"p0\":" + String(rec->lastTelem.rawP0) + ",";
        json += "\"p1\":" + String(rec->lastTelem.rawP1) + ",";
        json += "\"p2\":" + String(rec->lastTelem.rawP2) + ",";
        json += "\"rst\":" + String(rec->lastTelem.rawRst) + ",";
        json += "\"last_seen_sec\":" + String(ageSec) + ",";
        json += "\"packets\":" + String(rec->packetCount);
        json += "}";
    }
    json += "]}";

    s_server.send(200, "application/json", json);
}

static void handleApiRadio() {
    byte ver = ELECHOUSE_cc1101.SpiReadStatus(CC1101_VERSION);
    byte part = ELECHOUSE_cc1101.SpiReadStatus(CC1101_PARTNUM);
    byte marc = ELECHOUSE_cc1101.SpiReadStatus(CC1101_MARCSTATE);

    String json = "{";
    json += "\"chip_version\":\"0x" + String(ver, HEX) + "\",";
    json += "\"chip_partnum\":\"0x" + String(part, HEX) + "\",";
    json += "\"marcstate\":\"0x" + String(marc, HEX) + "\",";
    json += "\"miso_pin_level\":" + String(digitalRead(PIN_MISO)) + ",";
    json += "\"spi_ok\":" + String((ver == 0x04 || ver == 0x14) ? "true" : "false") + "}";

    s_server.send(200, "application/json", json);
}

static void handleApiScan() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        json += "{";
        json += "\"ssid\":\"" + WiFi.SSID(i) + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
        json += "\"secure\":" + String((WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? "true" : "false");
        json += "}";
    }
    json += "]";
    WiFi.scanDelete();
    s_server.send(200, "application/json", json);
}

static void handleApiSave() {
    if (!s_server.hasArg("plain")) {
        s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"No payload\"}");
        return;
    }

    String body = s_server.arg("plain");

    // Simple JSON parsing without heavy external libs
    auto extractJsonString = [](const String& json, const String& key) -> String {
        int keyPos = json.indexOf("\"" + key + "\"");
        if (keyPos == -1) return "";
        int colonPos = json.indexOf(':', keyPos);
        if (colonPos == -1) return "";
        int valStart = json.indexOf('"', colonPos);
        if (valStart == -1) return "";
        int valEnd = json.indexOf('"', valStart + 1);
        if (valEnd == -1) return "";
        return json.substring(valStart + 1, valEnd);
    };

    auto extractJsonInt = [](const String& json, const String& key, int defaultVal) -> int {
        int keyPos = json.indexOf("\"" + key + "\"");
        if (keyPos == -1) return defaultVal;
        int colonPos = json.indexOf(':', keyPos);
        if (colonPos == -1) return defaultVal;
        int valStart = colonPos + 1;
        while (valStart < (int)json.length() && (json[valStart] == ' ' || json[valStart] == '\t')) valStart++;
        int valEnd = valStart;
        while (valEnd < (int)json.length() && (isdigit(json[valEnd]) || json[valEnd] == '-')) valEnd++;
        if (valEnd > valStart) return json.substring(valStart, valEnd).toInt();
        return defaultVal;
    };

    String ssid = extractJsonString(body, "wifi_ssid");
    String pass = extractJsonString(body, "wifi_pass");
    String host = extractJsonString(body, "mqtt_host");
    int port    = extractJsonInt(body, "mqtt_port", 1883);
    String user = extractJsonString(body, "mqtt_user");
    String mpass= extractJsonString(body, "mqtt_pass");
    String topic= extractJsonString(body, "mqtt_topic");
    String key  = extractJsonString(body, "master_key");

    // MQTT first: a rejected topic must leave every setting untouched
    if (!ConfigManager::setMqtt(host.c_str(), (uint16_t)port, user.c_str(), mpass.c_str(), topic.length() ? topic.c_str() : "verisure")) {
        s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid MQTT topic (allowed: A-Z a-z 0-9 _ - and single '/')\"}");
        return;
    }
    if (ssid.length() > 0) {
        ConfigManager::setWifi(ssid.c_str(), pass.c_str());
    }

    if (key.length() == 32) {
        if (!ConfigManager::setMasterKeyHex(key.c_str())) {
            s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid Master Key (must be 32 hex characters)\"}");
            return;
        }
    }

    ConfigManager::save();
    s_server.send(200, "application/json", "{\"ok\":true}");

    s_rebootAt = millis() + 1500;
}

static void handleApiReboot() {
    s_server.send(200, "application/json", "{\"ok\":true}");
    s_rebootAt = millis() + 1000;
}

static void handleApiReset() {
    ConfigManager::resetToDefaults();
    s_server.send(200, "application/json", "{\"ok\":true}");
    s_rebootAt = millis() + 1000;
}

static const uint8_t S_MINIMAL_JPEG[] = {
    0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01,
    0x01, 0x01, 0x00, 0x48, 0x00, 0x48, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43,
    0x00, 0x03, 0x02, 0x02, 0x03, 0x02, 0x02, 0x03, 0x03, 0x03, 0x03, 0x04,
    0x06, 0x04, 0x04, 0x04, 0x04, 0x04, 0x08, 0x06, 0x06, 0x05, 0x06, 0x09,
    0x08, 0x0A, 0x0A, 0x09, 0x08, 0x09, 0x09, 0x0A, 0x0C, 0x0F, 0x0C, 0x0A,
    0x0B, 0x0E, 0x0B, 0x09, 0x09, 0x0D, 0x11, 0x0D, 0x0E, 0x0F, 0x10, 0x10,
    0x11, 0x10, 0x0A, 0x0C, 0x12, 0x13, 0x12, 0x10, 0x13, 0x0F, 0x10, 0x10,
    0x10, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x01, 0x00, 0x01, 0x01, 0x01,
    0x11, 0x00, 0xFF, 0xC4, 0x00, 0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09,
    0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00, 0x37, 0xFF, 0xD9
};

static void handleApiCameraBmp() {
    size_t len = 0;
    const uint8_t* bmp = imageReceiverBmp(&len);
    if (!bmp) {
        s_server.send(404, "text/plain", "no picture yet");
        return;
    }
    s_server.sendHeader("X-Tiles", String(imageReceiverTilesHave()) + "/" + String(RAW_TILES));
    s_server.send_P(200, "image/bmp", (const char*)bmp, len);
}

static void handleApiCameraJpg() {
    size_t len = 0;
    uint16_t missing = 0;
    const uint8_t* jpg = imageReceiverJpeg(&len, &missing);
    if (!jpg) {
        s_server.send(404, "text/plain", "no picture yet");
        return;
    }
    s_server.sendHeader("X-Missing-Chunks", String(missing));
    s_server.send_P(200, "image/jpeg", (const char*)jpg, len);
}

static void handleApiCameraLargeJpg() {
    size_t len = 0;
    uint16_t missing = 0;
    const uint8_t* jpg = imageReceiverJpegLarge(&len, &missing);
    if (!jpg) {
        s_server.send(404, "text/plain", "no large picture yet");
        return;
    }
    s_server.sendHeader("X-Missing-Chunks", String(missing));
    s_server.send_P(200, "image/jpeg", (const char*)jpg, len);
}

static void handleApiCameraTest() {
    String devId = "2NCEKS3T";
    if (s_server.hasArg("id")) {
        devId = s_server.arg("id");
    }
    // the ID becomes an MQTT topic level: 8 alphanumeric characters only
    bool idOk = devId.length() == 8;
    for (unsigned i = 0; idOk && i < devId.length(); i++) idOk = isalnum((unsigned char)devId[i]);
    if (!idOk) {
        s_server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid device ID\"}");
        return;
    }
    bool ok = mqttPublishCameraImage(devId.c_str(), S_MINIMAL_JPEG, sizeof(S_MINIMAL_JPEG));
    if (ok) {
        s_server.send(200, "application/json", "{\"status\":\"ok\",\"published\":true}");
    } else {
        s_server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"MQTT publish failed\"}");
    }
}

void webPortalInit() {
    sensorRegistryInit();
    ConfigManager::begin();
    const GatewayConfig& cfg = ConfigManager::get();

    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
            Serial.println("[WiFi-Event] Connected to AP (Link established)");
        } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
            Serial.printf("[WiFi-Event] IP assigned via DHCP: %s | RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
            s_isApMode = false;
            MDNS.begin("verisure-gateway");
        } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
            Serial.printf("[WiFi-Event] Disconnected from Wi-Fi. Reason: %d\n", info.wifi_sta_disconnected.reason);
        }
    });

    s_isApMode = false;
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);

    if (cfg.wifiSsid[0]) {
        s_wifiMulti.addAP(cfg.wifiSsid, cfg.wifiPass);
        Serial.printf("[Wi-Fi] Connecting to configured SSID: %s...\n", cfg.wifiSsid);
        s_wifiMulti.run(5000);
    } else {
        Serial.println("[Wi-Fi] No Wi-Fi SSID configured. Starting Access Point 'Verisure-Gateway'...");
        WiFi.mode(WIFI_AP);
        WiFi.softAP("Verisure-Gateway", "12345678");
        s_dnsServer.start(53, "*", WiFi.softAPIP());
        s_isApMode = true;
        Serial.printf("[Wi-Fi] AP started! IP: %s (Password: 12345678)\n", WiFi.softAPIP().toString().c_str());
    }

    // Web routes
    s_server.on("/", HTTP_GET, handleRoot);
    s_server.on("/api/status", HTTP_GET, handleApiStatus);
    s_server.on("/api/radio", HTTP_GET, handleApiRadio);
    s_server.on("/api/scan", HTTP_GET, handleApiScan);
    s_server.on("/api/save", HTTP_POST, handleApiSave);
    s_server.on("/api/reboot", HTTP_POST, handleApiReboot);
    s_server.on("/api/reset", HTTP_POST, handleApiReset);
    s_server.on("/api/camera.bmp", HTTP_GET, handleApiCameraBmp);
    s_server.on("/api/camera.jpg", HTTP_GET, handleApiCameraJpg);
    s_server.on("/api/camera_large.jpg", HTTP_GET, handleApiCameraLargeJpg);
    s_server.on("/api/camera_test", HTTP_GET, handleApiCameraTest);
    s_server.on("/api/camera_test", HTTP_POST, handleApiCameraTest);

    // Captive portal probes
    s_server.on("/generate_204", HTTP_GET, handleRoot);
    s_server.on("/hotspot-detect.html", HTTP_GET, handleRoot);
    s_server.on("/ncsi.txt", HTTP_GET, handleRoot);
    s_server.on("/canonical.html", HTTP_GET, handleRoot);
    s_server.onNotFound(handleRoot);

    s_server.begin();
    Serial.println("[WebPortal] HTTP server started on port 80.");
}

void webPortalLoop(int currentRssi, uint8_t marcstate, uint32_t totalPackets) {
    s_cachedRssi = currentRssi;
    s_cachedMarc = marcstate;
    s_cachedPackets = totalPackets;

    s_server.handleClient();

    if (s_isApMode) {
        s_dnsServer.processNextRequest();
    }

    // Non-blocking Wi-Fi monitor & auto-reconnect
    const wl_status_t status = WiFi.status();
    static bool wasConnected = false;
    static unsigned long lastAttempt = millis();
    const unsigned long reconnectInterval = 10000;

    if (status == WL_CONNECTED) {
        if (!wasConnected) {
            wasConnected = true;
            s_isApMode = false;
            Serial.printf("[Wi-Fi] Connected to \"%s\"! IP: %s | RSSI: %d dBm\n",
                          WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
            MDNS.begin("verisure-gateway");
        }
    } else {
        wasConnected = false;
        if (millis() - lastAttempt >= reconnectInterval) {
            lastAttempt = millis();
            Serial.println("[Wi-Fi] Attempting connection via WiFiMulti...");
            s_wifiMulti.run(3000);
        }
    }

    if (s_rebootAt > 0 && millis() >= s_rebootAt) {
        Serial.println("[System] Rebooting ESP32 now...");
        delay(100);
        ESP.restart();
    }
}

bool webPortalIsApMode() {
    return s_isApMode;
}

String webPortalGetIp() {
    if (s_isApMode) return WiFi.softAPIP().toString();
    if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
    return "Connecting...";
}
