# Security Policy

## 🔒 Operational Security & Best Practices

This project handles wireless alarm hardware and radio transmissions. While we implement strong AES-128 encryption with per-device key derivation and anti-replay counters, you must follow these best practices for production deployments:

### 1. Generate Your Own Master AES Key
* **Never** use the public example key of `secrets.h.example` (`0102030405060708090A0B0C0D0E0F10`): anyone can make packets your gateway accepts with it. The gateway warns at boot and on its web page while it is in use.
* Generate a secure random 128-bit master key using the built-in tool:
  ```bash
  python derive_key.py --generate-master
  ```
* Save this key in `secrets.h` on the gateway and use it when generating device keys with `derive_key.py`.
* Prefer `secrets.h` over typing the key on the setup access point's page: the access point `Verisure-Gateway` has a password unique to each gateway (derived from its MAC, or `DEFAULT_AP_PASS` in `secrets.h`; shown only on the serial console), but its page is plain HTTP, so anyone who knows or cracks that password and is in Wi-Fi range can read what you send. Do the setup where nobody else can listen.

### 2. Keep Credentials Local
* `VerisureDoorSensor/config.h`, `MD_CAM/firmware/config.h` and `868Receiver/src/secrets.h` are git-ignored.
* Photos committed to the repository must have their metadata removed first (phone photos carry
  the GPS position in EXIF).
* **Never force-add or commit these files to a public repository!**

### 3. Network Isolation
* Place the ESP32-S2 Gateway and your MQTT broker on an isolated **IoT VLAN** or guest network without direct access to sensitive internal network shares.
* **Web portal password:** the gateway's page and API (settings, master key, camera pictures) need the portal password (8-64 characters, HTTP Basic auth, any user name). It is set on the first save in the setup access point, with the serial command `portalpass <password>`, or with `DEFAULT_PORTAL_PASS` in `secrets.h`; only a salted hash is stored. Basic auth over plain HTTP sends the password readably with every request, so anyone on the same network can capture it: use the page only from a trusted LAN. Five wrong passwords lock the check for 30 s. Changes also need a POST with the header `X-Gateway: 1` from the gateway's own host (CSRF), and requests with a foreign `Host` are refused. `/api/replay` (a packet counter the build scripts read) stays public.
* **MQTT is plain TCP:** the gateway has no TLS/MQTTS support, so the MQTT user name, password and all messages (including camera pictures) cross the network unencrypted. Use a broker account that is only for the gateway and keep the broker on the trusted LAN.

### 4. Physical Access
* The gateway keeps the master key **unencrypted** in its flash (NVS, and in the firmware image when it is built in from `secrets.h`). Anyone with USB access to the gateway can read it out, and with it derive every device key. Keep the gateway where others cannot reach it.
* Flashed sensors and cameras stay **debug-unlocked**: anyone who opens one and connects to its debug header can read its device key (only that device's key, not the master key).

---

## 🛡️ Reporting a Vulnerability

If you discover a security vulnerability or cryptographic flaw within this project:
1. Please **do not** open a public GitHub issue.
2. Contact the maintainers directly or use [GitHub Private Vulnerability Reporting](https://docs.github.com/en/code-security/security-advisories/guidance-on-reporting-and-communicating/privately-reporting-a-security-vulnerability).
3. Provide a detailed description of the vulnerability, steps to reproduce, and potential impact.

We take security seriously and will investigate and address reported vulnerabilities promptly.
