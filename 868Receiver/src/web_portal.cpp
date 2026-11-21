#include "web_portal.h"
#include "image_receiver.h"
#include "config_manager.h"
#include "sensor_registry.h"
#include "hardware_pins.h"
#include "mqtt_manager.h"
#include "crypto.h"
#include "downlink.h"
#include "ha_discovery.h"
#include "portal_util.h"     // hostOnly, jsonGetString, jsonGetInt
#include "ota_update.h"
#include <WiFi.h>
#include <GuardedWebServer.h>   // not the core's <WebServer.h>: bounded run time per request (lib/GuardedWebServer)
#include <ESPmDNS.h>
#include <DNSServer.h>
#include "radio_task.h"
#include "esp_wifi.h"
#include <mbedtls/base64.h>

static GuardedWebServer s_server(80);
static DNSServer s_dnsServer;
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

  <div id="warn-card" class="card" style="display:none; border-color: var(--danger); color: var(--danger); font-size: 14px;"></div>

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
            <th>Contact / Motion</th>
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

  <!-- Camera (shown once a camera has been heard) -->
  <div id="camera-card" class="card" style="display:none;">
    <h2>&#128247; Camera <span id="cam-id"></span></h2>
    <img id="cam-img" alt="" style="max-width:100%; border-radius:8px; display:none;"
         onload="this.style.display='block'; document.getElementById('cam-none').style.display='none';"
         onerror="this.style.display='none'; document.getElementById('cam-none').style.display='block';">
    <div id="cam-none" style="color:var(--text-muted); font-size:13px;">No picture since the gateway started.</div>
    <div id="cam-info" style="color:var(--text-muted); font-size:13px; margin:8px 0;"></div>
    <div id="cam-msg" style="font-size:13px; margin-bottom:8px;"></div>
    <button type="button" onclick="takePicture('D1')">Take picture</button>
    <button type="button" class="secondary" onclick="takePicture('D4')">Flash picture</button>
    <div style="color:var(--text-muted); font-size:12px; margin-top:6px;">The camera picks the request up with its next poll (every ~30 s armed, ~2 min disarmed).</div>
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
          <input type="password" id="wifi_pass" name="wifi_pass" placeholder="Leave blank to keep the saved one (or for an open network)">
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
          <input type="password" id="mqtt_pass" name="mqtt_pass" placeholder="Optional; blank keeps the saved one (only while the host stays the same)">
          <button type="button" class="secondary" onclick="togglePass('mqtt_pass')">&#128065;</button>
        </div>
      </div>

      <div class="form-group">
        <label>MQTT Topic Prefix</label>
        <input type="text" id="mqtt_topic" name="mqtt_topic" value="verisure">
      </div>

      <div class="form-group full">
        <label>Web Portal Password (8&ndash;64 characters)</label>
        <div class="input-row">
          <input type="password" id="portal_pass" name="portal_pass" minlength="8" maxlength="64" autocomplete="new-password" placeholder="Leave blank to keep the current one">
          <button type="button" class="secondary" onclick="togglePass('portal_pass')">&#128065;</button>
        </div>
        <small style="color: var(--text-muted); font-size: 11px; margin-top: 4px;">
          Asked for by the browser (any user name). It protects the settings, the master key and the camera.
        </small>
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

  <!-- Firmware update over Wi-Fi -->
  <div class="card">
    <h2>&#11014;&#65039; Firmware Update</h2>
    <div id="fw-info" style="color:var(--text-muted); font-size:13px; margin-bottom:10px;"></div>
    <div class="input-row">
      <input type="file" id="fw-file" accept=".bin,application/octet-stream">
      <button type="button" onclick="uploadFirmware()">Upload &amp; Reboot</button>
    </div>
    <div id="fw-msg" style="font-size:13px; margin-top:8px;"></div>
    <small style="color: var(--text-muted); font-size: 11px;">
      The gateway's <code>firmware.bin</code> (<code>.pio/build/lolin_s2_mini/</code>). It is checked before it is
      activated; a new firmware that does not reach Wi-Fi within 5 minutes is rolled back.
    </small>
  </div>
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

// Every state-changing request carries X-Gateway: a page on another site cannot send that header
// without a CORS preflight, which the gateway never answers (CSRF protection).
function post(url, json) {
  const opts = { method: 'POST', headers: { 'X-Gateway': '1' } };
  if (json !== undefined) {
    opts.headers['Content-Type'] = 'application/json';
    opts.body = JSON.stringify(json);
  }
  return fetch(url, opts);
}

// The firmware upload, like post() with X-Gateway, but with upload progress (XMLHttpRequest)
function upload(url, file, onProgress) {
  return new Promise((resolve, reject) => {
    const x = new XMLHttpRequest();
    x.open('POST', url);
    x.setRequestHeader('X-Gateway', '1');
    x.setRequestHeader('Content-Type', 'application/octet-stream');
    x.upload.onprogress = e => { if (e.lengthComputable) onProgress(e.loaded, e.total); };
    x.onload = () => resolve({ status: x.status, text: x.responseText });
    x.onerror = () => reject(new Error('network error'));
    x.send(file);
  });
}

async function uploadFirmware() {
  const f = document.getElementById('fw-file').files[0];
  const msg = document.getElementById('fw-msg');
  if (!f) { msg.textContent = 'Choose a firmware .bin file first.'; return; }
  if (!confirm('Install ' + f.name + ' (' + f.size + ' bytes) and reboot the gateway?')) return;
  msg.style.color = 'var(--text)';
  try {
    const r = await upload('/api/ota', f, (n, t) => {
      msg.textContent = n < t ? 'Uploading... ' + Math.floor(100 * n / t) + ' %' : 'Checking the image...';
    });
    let reply = {};
    try { reply = JSON.parse(r.text); } catch (e) {}
    if (r.status === 200 && reply.ok) {
      msg.style.color = 'var(--success)';
      msg.textContent = 'Firmware installed, the gateway reboots. Reloading in 15 s...';
      setTimeout(() => { location.reload(); }, 15000);
    } else {
      msg.style.color = 'var(--danger)';
      msg.textContent = 'Update refused (HTTP ' + r.status + '): ' + (reply.error || r.text) + '. The running firmware is unchanged.';
    }
  } catch (err) {
    msg.style.color = 'var(--danger)';
    msg.textContent = 'Upload failed (connection lost). The running firmware is unchanged.';
  }
}

function showWarnings(list) {
  const w = document.getElementById('warn-card');
  w.textContent = list.join(' ');
  w.style.display = list.length ? 'block' : 'none';
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
    if (res.status === 401 || res.status === 429) {
      // a wrong or changed password: stop polling, or the page would lock this client out
      clearInterval(statusTimer);
      showWarnings(['Wrong or changed web portal password (or too many tries): reload the page to log in again.']);
      return;
    }
    if (res.status === 403) {
      const r = await res.json();
      if (r.error === 'no_portal_password')
        showWarnings([r.message]);
      return;
    }
    if (!res.ok) return;
    const data = await res.json();
    const warn = [];
    if (data.config.master_key_example)
      warn.push('The master key is the public example key: anyone can make packets this gateway accepts. Set your own key (python derive_key.py --generate-master).');
    if (!data.config.portal_password_set)
      warn.push('Set a web portal password below (required before the gateway leaves setup mode).');
    showWarnings(warn);
    document.getElementById('portal_pass').required = !data.config.portal_password_set;

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
    if (data.firmware)
      document.getElementById('fw-info').textContent = 'Running: slot ' + data.firmware.partition + ' (' +
        data.firmware.state + '), MD5 ' + data.firmware.md5;

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
        const magBadge = s.camera
          ? (s.magnet ? '<span class="badge badge-alarm">MOTION</span>' : '<span class="badge badge-ok">No motion</span>')
          : (s.magnet ? '<span class="badge badge-alarm">OPEN (ALARM)</span>' : '<span class="badge badge-ok">CLOSED</span>');
        const shockBadge = s.camera ? '&mdash;'
          : (s.shock ? '<span class="badge badge-alarm">SHOCK!</span>' : '<span class="badge badge-ok">Clear</span>');
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
    updateCamera(data);
  } catch (err) {
    console.error(err);
  }
}

let camId = null, camLoaded = 0;
function updateCamera(data) {
  const cam = data.sensors.find(s => s.camera);
  document.getElementById('camera-card').style.display = cam ? 'block' : 'none';
  if (!cam) return;
  camId = cam.id;
  document.getElementById('cam-id').textContent = cam.id;
  document.getElementById('cam-info').textContent =
    'Alarm ' + (data.armed ? 'armed' : 'disarmed') + ' | last poll ' + cam.last_seen_sec + ' s ago';
  if (Date.now() - camLoaded > 15000) {         // the newest picture, at most every 15 s
    camLoaded = Date.now();
    document.getElementById('cam-img').src = '/api/camera.jpg?t=' + camLoaded;
  }
}

async function takePicture(cmd) {
  if (!camId) return;
  const info = document.getElementById('cam-msg');
  try {
    const res = await post('/api/picture?id=' + encodeURIComponent(camId) + '&cmd=' + cmd);
    info.textContent = res.ok ? cmd + ' requested; the picture appears here when it has arrived.' : 'Request failed.';
    if (res.ok) camLoaded = Date.now() - 5000;   // look again in ~10 s
  } catch (err) {
    info.textContent = 'Request failed.';
  }
}

async function scanWifi() {
  const box = document.getElementById('scan-results');
  box.style.display = 'block';
  box.innerHTML = '<div style="padding:10px; color:var(--text-muted); font-size:13px;">Scanning for networks...</div>';
  try {
    let list = null;
    for (let i = 0; i < 40 && !Array.isArray(list); i++) {   // the scan runs in the background
      if (i) await new Promise(r => setTimeout(r, 500));
      list = await (await fetch('/api/scan')).json();
    }
    if (!Array.isArray(list)) throw new Error('scan timeout');
    if (list.length === 0) {
      box.innerHTML = '<div style="padding:10px; color:var(--text-muted); font-size:13px;">No networks found.</div>';
      return;
    }
    // SSIDs come from anyone nearby: insert them as text, never as HTML
    box.innerHTML = '';
    list.forEach(n => {
      const item = document.createElement('div');
      item.className = 'scan-item';
      item.addEventListener('click', () => selectWifi(n.ssid));
      const name = document.createElement('span');
      const strong = document.createElement('strong');
      strong.textContent = n.ssid;
      name.appendChild(strong);
      if (n.secure) name.insertAdjacentHTML('beforeend', ' &#128274;');
      const rssi = document.createElement('span');
      rssi.style.color = 'var(--text-muted)';
      rssi.textContent = n.rssi + ' dBm';
      item.appendChild(name);
      item.appendChild(rssi);
      box.appendChild(item);
    });
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
  json.mqtt_port = parseInt(json.mqtt_port, 10) || 1883;   // a number, not a string

  try {
    const res = await post('/api/save', json);
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
    await post('/api/reboot');
    showToast('Rebooting gateway... Reloading page in 5 seconds.', false);
    setTimeout(() => { location.reload(); }, 6000);
  } catch (err) {}
}

async function factoryReset() {
  if (!confirm('WARNING: This erases the stored settings in NVS: Wi-Fi, MQTT, master key and the web portal password (the replay counters stay, and settings built into secrets.h come back). Without a portal password the page is locked until one is set on the serial console (portalpass) or in the setup access point. Continue?')) return;
  try {
    await post('/api/reset');
    showToast('NVS erased. Rebooting gateway...', false);
    setTimeout(() => { location.reload(); }, 6000);
  } catch (err) {}
}

const statusTimer = setInterval(fetchStatus, 2000);
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

static String jsonEscape(const String& in) {
    String out;
    out.reserve(in.length() + 8);
    for (unsigned i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((unsigned char)c < 0x20) { char b[7]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += c;
    }
    return out;
}

/* ============================================================================
 * Access control
 * ============================================================================
 * - Host check: the Host header must name the gateway (its IP or verisure-gateway[.local]), so
 *   a DNS-rebinding page cannot reach the API under its own name.
 * - Password: HTTP Basic auth (any user name) against the portal password, whose salted hash
 *   is in NVS. Without one the API only works in the setup access point (first setup, which
 *   sets it); outside it the page shows how to set one (serial: portalpass <password>).
 *   Five wrong passwords in a row lock the check for 30 s.
 * - CSRF: state-changing requests must be POST with the header X-Gateway: 1 (a cross-site page
 *   cannot add it without a CORS preflight, which is never answered) and, if the browser sends
 *   an Origin, it must be this host.
 * Basic auth over plain HTTP can be read by anyone on the same network: see SECURITY.md. */
enum Access { ACCESS_PUBLIC, ACCESS_READ, ACCESS_WRITE };

static const char* COLLECT_HEADERS[] = {"Authorization", "Origin", "X-Gateway", "X-OTA-MD5"};
// Wrong passwords are counted per client IP: 5 within 10 min lock that client for 30 s. A login
// by another client (the owner's open page polls every 2 s) does not reset an attacker's count,
// and one client cannot lock out the others. Times use millis() differences (wrap-safe).
#define AUTH_SLOTS      8
#define AUTH_MAX_FAILS  5
#define AUTH_LOCK_MS    30000UL
#define AUTH_WINDOW_MS  600000UL
struct AuthFailSlot {
    uint32_t ip;
    uint8_t fails;
    bool locked;
    unsigned long since;    // start of the failure window, or of the lock
};
static AuthFailSlot s_authSlots[AUTH_SLOTS];

static AuthFailSlot* authSlot(IPAddress ip) {
    uint32_t a = (uint32_t)ip;
    AuthFailSlot* freeSlot = nullptr;
    AuthFailSlot* victim = nullptr;
    for (auto& s : s_authSlots) {
        if (s.ip == a) return &s;
        if (!s.locked && s.fails == 0) {
            if (!freeSlot) freeSlot = &s;
        } else if (!s.locked && (!victim || millis() - s.since > millis() - victim->since)) {
            victim = &s;                      // the oldest unlocked failure window
        }
    }
    // a free slot, else the oldest unlocked one; a lock is only dropped if all 8 slots are locked
    AuthFailSlot* r = freeSlot ? freeSlot : (victim ? victim : &s_authSlots[0]);
    r->ip = a;
    r->fails = 0;
    r->locked = false;
    r->since = millis();
    return r;
}

static bool hostAllowed() {
    String h = hostOnly(s_server.hostHeader());
    if (h.length() == 0) return true;         // HTTP/1.0 client without Host (not a browser)
    if (h == "verisure-gateway.local" || h == "verisure-gateway") return true;
    if (s_isApMode && h == WiFi.softAPIP().toString()) return true;
    return WiFi.status() == WL_CONNECTED && h == WiFi.localIP().toString();
}

static bool originAllowed() {
    String o = s_server.header("Origin");
    if (o.length() == 0) return true;         // not sent by every client; X-Gateway still applies
    int p = o.indexOf("://");
    if (p < 0) return false;
    return hostOnly(o.substring(p + 3)) == hostOnly(s_server.hostHeader());
}

static bool passwordFromHeader() {
    String a = s_server.header("Authorization");
    if (!a.startsWith("Basic ")) return false;
    String b64 = a.substring(6);
    b64.trim();
    unsigned char dec[160];
    size_t n = 0;
    if (b64.length() > 200 ||
        mbedtls_base64_decode(dec, sizeof(dec) - 1, &n, (const unsigned char*)b64.c_str(), b64.length()) != 0)
        return false;
    dec[n] = 0;
    char* colon = strchr((char*)dec, ':');
    bool ok = colon && ConfigManager::checkPortalPassword(colon + 1);
    memset(dec, 0, sizeof(dec));
    return ok;
}

/* /api/ota checks access when the body starts, before the upload is read; its reply can only be
 * sent after the body, so authorize() then keeps the reply here instead of sending it. */
static bool s_deferReply = false;
static int s_deferredCode = 0;
static const char* s_deferredBody = "";
static bool s_deferredAuthHeader = false;

static void authReply(int code, const char* body, bool authHeader = false) {
    if (s_deferReply) {
        s_deferredCode = code;
        s_deferredBody = body;
        s_deferredAuthHeader = authHeader;
        return;
    }
    if (authHeader) s_server.sendHeader("WWW-Authenticate", "Basic realm=\"Verisure Gateway\", charset=\"UTF-8\"");
    s_server.send(code, "application/json", body);
}

/** @return true if the request may go on; otherwise the error reply has been sent (or deferred). */
static bool authorize(Access access) {
    if (!hostAllowed()) {
        authReply(403, "{\"error\":\"bad_host\"}");
        return false;
    }
    if (access == ACCESS_WRITE && (s_server.header("X-Gateway") != "1" || !originAllowed())) {
        authReply(403, "{\"error\":\"csrf\",\"message\":\"POST with the header X-Gateway: 1 from this host\"}");
        return false;
    }
    if (access == ACCESS_PUBLIC) return true;
    if (!ConfigManager::hasPortalPassword()) {
        if (s_isApMode) return true;          // first setup in the setup access point
        authReply(403,
                  "{\"error\":\"no_portal_password\",\"message\":\"No web portal password is set. Set one on the "
                  "gateway's serial console (portalpass <password>) or in the setup access point.\"}");
        return false;
    }
    AuthFailSlot* slot = authSlot(s_server.client().remoteIP());
    if (slot->locked) {
        if (millis() - slot->since < AUTH_LOCK_MS) {
            authReply(429, "{\"error\":\"locked\",\"message\":\"Too many wrong passwords, wait 30 s\"}");
            return false;
        }
        slot->locked = false;                 // lock over: start counting again
        slot->fails = 0;
    }
    if (passwordFromHeader()) {
        slot->fails = 0;                      // only this client's count; others keep theirs
        return true;
    }
    if (s_server.header("Authorization").length()) {
        if (slot->fails == 0 || millis() - slot->since > AUTH_WINDOW_MS) {
            slot->fails = 0;
            slot->since = millis();           // a new window of failures
        }
        if (++slot->fails >= AUTH_MAX_FAILS) {
            slot->locked = true;
            slot->since = millis();
            Serial.printf("[WebPortal] %u wrong portal passwords from %s: locked for 30 s\n",
                          (unsigned)AUTH_MAX_FAILS, s_server.client().remoteIP().toString().c_str());
        }
    }
    authReply(401, "{\"error\":\"auth\"}", true);
    return false;
}

static void handleRoot() {
    if (isCaptivePortalRequest()) {
        s_server.sendHeader("Location", "http://192.168.4.1/", true);
        s_server.send(302, "text/plain", "");
        return;
    }
    // without a password the static page is shown (it explains how to set one); the API stays shut
    if (!authorize(ConfigManager::hasPortalPassword() ? ACCESS_READ : ACCESS_PUBLIC)) return;
    s_server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

/** GET /api/replay?id=XXXXXXXX: the device's last accepted persistent counter (not secret; the
 *  door sensor's build script starts a reflashed sensor above it). */
static void handleApiReplay() {
    if (!authorize(ACCESS_PUBLIC)) return;
    String id = s_server.arg("id");
    bool valid = id.length() == 8;
    for (unsigned i = 0; valid && i < 8; i++) valid = isalnum((unsigned char)id[i]);
    if (!valid) {
        s_server.send(400, "application/json", "{\"error\":\"id must be 8 letters/digits\"}");
        return;
    }
    uint32_t ctr = 0;
    bool ok = cryptoReplayFloor(id.c_str(), &ctr);
    String json = "{\"id\":\"" + id + "\",\"known\":" +
                  String(ok ? "true" : "false") + ",\"counter\":" + String((unsigned long)ctr) + "}";
    s_server.send(200, "application/json", json);
}

static void handleApiStatus() {
    if (!authorize(ACCESS_READ)) return;
    const GatewayConfig& cfg = ConfigManager::get();
    unsigned long now = millis();

    String json = "{";
    json += "\"wifi\":{";
    json += "\"mode\":\"" + String(s_isApMode ? "AP" : "STA") + "\",";
    json += "\"connected\":" + String((WiFi.status() == WL_CONNECTED) ? "true" : "false") + ",";
    json += "\"ssid\":\"" + jsonEscape(s_isApMode ? "Verisure-Gateway" : cfg.wifiSsid) + "\",";
    json += "\"ip\":\"" + (s_isApMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\",";
    json += "\"rssi\":" + String(s_isApMode ? 0 : WiFi.RSSI()) + "},";

    json += "\"radio\":{";
    json += "\"rssi\":" + String(s_cachedRssi) + ",";
    json += "\"marcstate\":\"0x" + String(s_cachedMarc, HEX) + "\",";
    json += "\"total_packets\":" + String(s_cachedPackets) + "},";

    json += "\"radio_diag\":{";   // radio task counters since boot (radio_task.h)
    radioDiagAppendJson(json);
    json += "},";

    json += "\"diag\":{";       // counters since boot (gateway_diag.h)
    gatewayDiagAppendJson(json, gatewayDiagSample());
    json += "},";

    json += "\"mqtt\":{";
    json += "\"connected\":" + String(mqttIsConnected() ? "true" : "false") + ",";
    json += "\"status\":\"" + jsonEscape(mqttGetStatusString()) + "\"},";

    json += "\"config\":{";
    json += "\"wifi_ssid\":\"" + jsonEscape(cfg.wifiSsid) + "\",";
    json += "\"mqtt_host\":\"" + jsonEscape(cfg.mqttHost) + "\",";
    json += "\"mqtt_port\":" + String(cfg.mqttPort) + ",";
    json += "\"mqtt_user\":\"" + jsonEscape(cfg.mqttUser) + "\",";
    json += "\"mqtt_topic\":\"" + jsonEscape(cfg.mqttTopic) + "\",";
    // The master key is never sent back to the browser, only whether one is set.
    json += "\"master_key_set\":" + String(cfg.masterKeyHex[0] ? "true" : "false") + ",";
    json += "\"master_key_example\":" + String(ConfigManager::masterKeyIsExample() ? "true" : "false") + ",";
    json += "\"portal_password_set\":" + String(ConfigManager::hasPortalPassword() ? "true" : "false") + "},";

    json += "\"firmware\":{\"md5\":\"" + otaFirmwareMd5() + "\",\"partition\":\"" + otaRunningPartition() +
            "\",\"state\":\"" + otaAppState() + "\"},";
    json += "\"armed\":" + String(downlinkArmed() ? "true" : "false") + ",";
    json += "\"sensors\":[";
    int count = sensorRegistryGetCount();
    for (int i = 0; i < count; i++) {
        const SensorRecord* rec = sensorRegistryGetByIndex(i);
        if (!rec) continue;
        if (i > 0) json += ",";
        unsigned long ageSec = (now >= rec->lastSeenMs) ? (now - rec->lastSeenMs) / 1000 : 0;
        json += "{";
        json += "\"id\":\"" + String(rec->lastTelem.deviceId) + "\",";
        json += "\"camera\":" + String(haIsMotionCameraSensor(rec->lastTelem.deviceId) ? "true" : "false") + ",";
        json += "\"seq\":" + String(rec->lastTelem.counter) + ",";
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
    if (!authorize(ACCESS_READ)) return;
    // cached by the radio task (it owns the SPI bus): VERSION/PARTNUM from the last init or
    // probe, MARCSTATE from the last sample (at most RADIO_STATUS_INTERVAL_MS old while idle)
    const bool online = g_radioStatus.online;
    const uint8_t ver = g_radioStatus.version;
    const uint8_t part = g_radioStatus.partnum;
    const uint8_t marc = g_radioStatus.marcState;

    String json = "{";
    json += "\"chip_version\":\"0x" + String(ver, HEX) + "\",";
    json += "\"chip_partnum\":\"0x" + String(part, HEX) + "\",";
    json += "\"marcstate\":\"0x" + String(marc, HEX) + "\",";
    json += "\"spi_ok\":" + String((online && (ver == 0x04 || ver == 0x14)) ? "true" : "false") + "}";

    s_server.send(200, "application/json", json);
}

// Asynchronous scan: the first request starts it and gets 202 {"scanning":true}; the page asks
// again until the list is ready (a synchronous scan blocked the loop for ~2-3 s).
static void handleApiScan() {
    if (!authorize(ACCESS_READ)) return;
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_FAILED) {
        WiFi.scanNetworks(true);
        n = WIFI_SCAN_RUNNING;
    }
    if (n == WIFI_SCAN_RUNNING) {
        s_server.send(202, "application/json", "{\"scanning\":true}");
        return;
    }
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        json += "{";
        json += "\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
        json += "\"secure\":" + String((WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? "true" : "false");
        json += "}";
    }
    json += "]";
    WiFi.scanDelete();
    s_server.send(200, "application/json", json);
}

static void handleApiSave() {
    if (!authorize(ACCESS_WRITE)) return;
    if (!s_server.hasArg("plain")) {
        s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"No payload\"}");
        return;
    }

    String body = s_server.arg("plain");

    String ssid = jsonGetString(body, "wifi_ssid");
    String pass = jsonGetString(body, "wifi_pass");
    String host = jsonGetString(body, "mqtt_host");
    long port   = jsonGetInt(body, "mqtt_port", 1883);
    String portalPass = jsonGetString(body, "portal_pass");
    String user = jsonGetString(body, "mqtt_user");
    String mpass= jsonGetString(body, "mqtt_pass");
    String topic= jsonGetString(body, "mqtt_topic");
    String key  = jsonGetString(body, "master_key");

    if (port < 1 || port > 65535) {
        s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"MQTT port must be 1..65535\"}");
        return;
    }
    // the first setup must set a portal password; later an empty field keeps it
    if (portalPass.length() || !ConfigManager::hasPortalPassword()) {
        if (portalPass.length() < 8 || portalPass.length() > 64) {
            s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"The web portal password must be 8-64 characters\"}");
            return;
        }
    }
    uint8_t keyCheck[16];
    if (key.length() && (key.length() != 32 || !ConfigManager::hexToBytes(key.c_str(), keyCheck, 16))) {
        s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid Master Key (must be 32 hex characters)\"}");
        return;
    }

    // MQTT first: a rejected topic must leave every setting untouched
    // The passwords are never sent to the browser, so an empty field means "keep the saved one"
    // (a null pass keeps it). Wi-Fi: a new SSID with an empty field is an open network.
    if (!ConfigManager::setMqtt(host.c_str(), (uint16_t)port, user.c_str(), mpass.length() ? mpass.c_str() : nullptr,
                                topic.length() ? topic.c_str() : "verisure")) {
        s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid MQTT topic (allowed: A-Z a-z 0-9 _ - and single '/')\"}");
        return;
    }
    if (ssid.length() > 0) {
        bool sameSsid = ssid == ConfigManager::get().wifiSsid;
        ConfigManager::setWifi(ssid.c_str(), (pass.length() || !sameSsid) ? pass.c_str() : nullptr);
    }

    if (key.length() == 32) {
        if (!ConfigManager::setMasterKeyHex(key.c_str())) {
            s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid Master Key (must be 32 hex characters)\"}");
            return;
        }
    }

    if (portalPass.length()) ConfigManager::setPortalPassword(portalPass.c_str());
    ConfigManager::save();
    s_server.send(200, "application/json", "{\"ok\":true}");

    s_rebootAt = millis() + 1500;
}

static void handleApiReboot() {
    if (!authorize(ACCESS_WRITE)) return;
    s_server.send(200, "application/json", "{\"ok\":true}");
    s_rebootAt = millis() + 1000;
}

static void handleApiReset() {
    if (!authorize(ACCESS_WRITE)) return;
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

static void handleApiCameraJpg() {
    if (!authorize(ACCESS_READ)) return;
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
    if (!authorize(ACCESS_READ)) return;
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
    if (!authorize(ACCESS_WRITE)) return;
    if (!s_server.hasArg("id")) {
        s_server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Missing id (the camera's 8-character ID)\"}");
        return;
    }
    String devId = s_server.arg("id");
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

// POST /api/picture?id=<camera>&cmd=D1..D6: queue a picture request like MQTT picture/set
static void handleApiPicture() {
    if (!authorize(ACCESS_WRITE)) return;
    String devId = s_server.arg("id");
    String cmd = s_server.arg("cmd");
    bool ok = devId.length() == 8 && haIsMotionCameraSensor(devId.c_str()) && cmd.length() == 2 &&
              cmd[0] == 'D' && cmd[1] >= '1' && cmd[1] <= '6';
    for (unsigned i = 0; ok && i < devId.length(); i++) ok = isalnum((unsigned char)devId[i]);
    if (!ok) {
        s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"Need id=<camera ID> and cmd=D1..D6\"}");
        return;
    }
    uint8_t c = (uint8_t)(0xD0 + (cmd[1] - '0'));
    if (!downlinkRequestPicture(devId.c_str(), c)) {
        s_server.send(503, "application/json", "{\"ok\":false,\"error\":\"Request table full\"}");
        return;
    }
    Serial.printf("[WebPortal] Picture %s requested for %s\n", cmd.c_str(), devId.c_str());
    s_server.send(200, "application/json", "{\"ok\":true}");
}

/* POST /api/ota: firmware update. The image is the raw request body (Content-Type
 * application/octet-stream, Content-Length required), optional header X-OTA-MD5: <32 hex>.
 * The GuardedWebServer hands the body over in ~1.4 kB pieces (handleApiOtaBody) and calls
 * handleApiOta after the last one. Access is checked on the first piece, before anything is
 * written; a refused upload is answered at once and its connection closed. A dropped, stalled
 * (a piece not complete within 3 s) or too slow connection aborts the update. The running
 * firmware stays in every error case. The whole upload blocks loop(), so nothing in here (or in
 * the web server's wait loops) may block longer than the 5 s loop watchdog without feeding it,
 * and nothing may publish to MQTT: the radio task keeps queueing frames meanwhile (256 entries)
 * and loop() handles them after the upload (when it failed; a successful one reboots). */
static bool s_otaBody = false;          // this request's body came as a raw stream
static bool s_otaAllowed = false;
static bool s_otaReplied = false;       // refused at the start: reply sent, connection closed

/* A refused upload is answered at once and its connection closed, so the WebServer stops
 * reading the body: otherwise a client announcing a huge Content-Length and sending a byte every
 * few seconds would hold handleClient() (and so MQTT, Wi-Fi reconnect, the CLI) indefinitely,
 * without any password (review 2026-11-19). */
static void otaRefuse(int code, const String& body) {
    s_server.send(code, "application/json", body);
    s_otaReplied = true;
    s_otaAllowed = false;
    s_server.client().stop();
}

static void handleApiOtaBody() {
    if (!s_server.hasRaw()) return;     // multipart form: refused in handleApiOta
    HTTPRaw& raw = s_server.raw();
    switch (raw.status) {
        case RAW_START:
            // The web server gives a piece 3 s (GWS_RAW_PIECE_MS) to arrive completely and feeds the
            // loop watchdog while it waits, so a stalled or trickling client ends in RAW_ABORTED.
            s_otaBody = true;
            s_otaReplied = false;
            s_deferReply = true;
            s_otaAllowed = authorize(ACCESS_WRITE);
            s_deferReply = false;
            if (!s_otaAllowed) {
                authReply(s_deferredCode, s_deferredBody, s_deferredAuthHeader);
                s_otaReplied = true;
                s_server.client().stop();
            } else if (s_isApMode) {
                // in the setup AP (maybe without a portal password yet) no update: it also
                // could not be confirmed there (no station Wi-Fi) and would be rolled back
                otaRefuse(403, "{\"ok\":false,\"error\":\"No firmware update in the setup access point\"}");
            } else if (s_rebootAt) {
                // an update was just activated: a second one would overwrite the selected slot
                otaRefuse(409, "{\"ok\":false,\"error\":\"Rebooting into the new firmware, try again afterwards\"}");
            } else if (!otaBegin((size_t)max(0, s_server.clientContentLength()), s_server.header("X-OTA-MD5").c_str())) {
                otaRefuse(400, "{\"ok\":false,\"error\":\"" + jsonEscape(otaError()) + "\"}");
            }
            break;
        case RAW_WRITE:
            if (s_otaAllowed) otaWrite(raw.buf, raw.currentSize);
            break;
        case RAW_END:
            break;
        case RAW_ABORTED:               // connection lost (or closed by otaRefuse): nothing activated
            if (s_otaAllowed) otaAbort();
            s_otaBody = false;
            s_otaAllowed = false;
            s_otaReplied = false;
            break;
    }
    // the upload blocks loop() until it is complete: feed the loop task watchdog for every piece
    // (the OTA path must never be killed by it). Nothing else happens here: no radio frame
    // handling (it publishes to MQTT and prints, which can block), see the comment above.
    feedLoopWDT();
}

static void handleApiOta() {
    if (!s_otaBody) {
        if (!authorize(ACCESS_WRITE)) return;
        s_server.send(415, "application/json", "{\"ok\":false,\"error\":\"Send the image as the raw request body (Content-Type: application/octet-stream)\"}");
        return;
    }
    s_otaBody = false;
    if (s_otaReplied) {                 // already answered at the start
        s_otaReplied = false;
        return;
    }
    if (!s_otaAllowed) {
        authReply(s_deferredCode, s_deferredBody, s_deferredAuthHeader);
        return;
    }
    s_otaAllowed = false;
    feedLoopWDT();   // otaFinish(): MD5 check and esp_ota_set_boot_partition() verifying the whole image
    const bool otaOk = otaFinish();
    feedLoopWDT();
    if (!otaOk) {
        s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(otaError()) + "\"}");
        return;
    }
    s_server.send(200, "application/json", "{\"ok\":true,\"message\":\"Firmware written and verified, rebooting\"}");
    s_rebootAt = millis() + 1500;
}

void webPortalInit() {
    sensorRegistryInit();
    otaInit();
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
        Serial.printf("[Wi-Fi] Connecting to configured SSID: %s...\n", cfg.wifiSsid);
        WiFi.begin(cfg.wifiSsid, cfg.wifiPass);   // returns at once; setup() waits up to 8 s
    } else {
        Serial.println("[Wi-Fi] No Wi-Fi SSID configured. Starting Access Point 'Verisure-Gateway'...");
        WiFi.mode(WIFI_AP);
        // every gateway has its own AP password (see ConfigManager::apPassword): with a common
        // one, anyone who recorded the setup could read the settings typed on the page
        char apPass[64];
        ConfigManager::apPassword(apPass);
        WiFi.softAP("Verisure-Gateway", apPass);
        s_dnsServer.start(53, "*", WiFi.softAPIP());
        s_isApMode = true;
        Serial.printf("[Wi-Fi] AP started! IP: %s (Password: %s)\n", WiFi.softAPIP().toString().c_str(), apPass);
        memset(apPass, 0, sizeof(apPass));
    }

    // Web routes (access rules: authorize())
    s_server.collectHeaders(COLLECT_HEADERS, sizeof(COLLECT_HEADERS) / sizeof(COLLECT_HEADERS[0]));
    s_server.on("/", HTTP_GET, handleRoot);
    s_server.on("/api/status", HTTP_GET, handleApiStatus);
    s_server.on("/api/replay", HTTP_GET, handleApiReplay);
    s_server.on("/api/radio", HTTP_GET, handleApiRadio);
    s_server.on("/api/scan", HTTP_GET, handleApiScan);
    s_server.on("/api/save", HTTP_POST, handleApiSave);
    s_server.on("/api/reboot", HTTP_POST, handleApiReboot);
    s_server.on("/api/reset", HTTP_POST, handleApiReset);
    s_server.on("/api/camera.jpg", HTTP_GET, handleApiCameraJpg);
    s_server.on("/api/camera_large.jpg", HTTP_GET, handleApiCameraLargeJpg);
    s_server.on("/api/camera_test", HTTP_POST, handleApiCameraTest);
    s_server.on("/api/picture", HTTP_POST, handleApiPicture);
    s_server.onRawUpload("/api/ota", HTTP_POST, handleApiOta, handleApiOtaBody);   // the only route exempt from the 3 s request deadline

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
        // repeat the AP password on the serial console for whoever opens it after the boot
        static unsigned long lastApHint = 0;
        if (millis() - lastApHint >= 60000UL) {
            lastApHint = millis();
            char apPass[64];
            ConfigManager::apPassword(apPass);
            Serial.printf("[Wi-Fi] Setup access point 'Verisure-Gateway', password: %s, page http://192.168.4.1\n", apPass);
            memset(apPass, 0, sizeof(apPass));
        }
    }

    // Non-blocking Wi-Fi monitor & reconnect: WiFi.begin() returns at once, so the radio FIFO is
    // still read while the gateway reconnects (WiFiMulti.run() blocked the loop for up to 3 s)
    const wl_status_t status = WiFi.status();
    static bool wasConnected = false;
    static unsigned long lastAttempt = millis();
    const unsigned long reconnectInterval = 20000;   // the driver's auto-reconnect usually wins

    otaLoop(status == WL_CONNECTED);
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
        const GatewayConfig& cfg = ConfigManager::get();
        if (!s_isApMode && cfg.wifiSsid[0] && millis() - lastAttempt >= reconnectInterval) {
            lastAttempt = millis();
            Serial.println("[Wi-Fi] Not connected, starting a new connection attempt...");
            WiFi.disconnect();
            WiFi.begin(cfg.wifiSsid, cfg.wifiPass);
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
