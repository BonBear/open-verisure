---
name: Bug report
about: Something does not work as described
labels: bug
---

**What happened, and what did you expect?**

**Which part:** gateway / door sensor / camera / scripts / Home Assistant

**Versions:** commit or release of this project, SDCC version (`sdcc --version`), PlatformIO
platform if you changed it.

**Gateway log:** the lines around the problem from the serial console (`status` output helps).
Remove anything private first. The gateway never prints keys, only their check value; never post
your master key, a device key, `secrets.h`, `config.h` or Wi-Fi/MQTT passwords.

**Steps to reproduce:**
