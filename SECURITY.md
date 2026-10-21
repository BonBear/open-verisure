# Security Policy

## 🔒 Operational Security & Best Practices

This project handles wireless alarm hardware and radio transmissions. While we implement strong AES-128 encryption with per-device key derivation and anti-replay counters, you must follow these best practices for production deployments:

### 1. Generate Your Own Master AES Key
* **DO NOT** use the default development master key (`0102030405060708090A0B0C0D0E0F10`) in real installations!
* Generate a secure random 128-bit master key using the built-in tool:
  ```bash
  python derive_key.py --generate-master
  ```
* Save this key in `secrets.h` on the gateway and use it when generating device keys with `derive_key.py`.

### 2. Keep Credentials Local
* `VerisureDoorSensor/config.h` and `868Receiver/src/secrets.h` are git-ignored.
* **Never force-add or commit these files to a public repository!**

### 3. Network Isolation
* Place the ESP32-S2 Gateway and your MQTT broker on an isolated **IoT VLAN** or guest network without direct access to sensitive internal network shares.
* Use strong credentials and TLS/MQTTS where applicable for your MQTT broker.

---

## 🛡️ Reporting a Vulnerability

If you discover a security vulnerability or cryptographic flaw within this project:
1. Please **do not** open a public GitHub issue.
2. Contact the maintainers directly or use [GitHub Private Vulnerability Reporting](https://docs.github.com/en/code-security/security-advisories/guidance-on-reporting-and-communicating/privately-reporting-a-security-vulnerability).
3. Provide a detailed description of the vulnerability, steps to reproduce, and potential impact.

We take security seriously and will investigate and address reported vulnerabilities promptly.
