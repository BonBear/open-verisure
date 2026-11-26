# Project Memory & Knowledge Bank (Index)

This is the persistent long-term memory of the agents and developers across sessions. It is split by topic under `.agents/memory/` so each task loads **only the 1–2 relevant files** instead of the entire bank.

## Memory Rules & Hygiene
1. **Confirmed facts only:** Never save hypotheses or guesses — only tested, verified insights.
2. **Compact format:** Short, precise bullet points:
   `- [YYYY-MM-DD] [AREA/COMPONENT]: Concrete rule, limitation or lesson learned.`
3. **No log dumping:** Never paste source code blocks or raw stack traces.
4. **Topical files:** Add each new entry to its matching file below (newest last). When a later finding supersedes an entry, rewrite or shorten the old one instead of leaving both.
5. **Selective reading:** Open **only** the topic file(s) matching your current task. Never read unrelated component files.

## Topic Files

| Topic / Area | File | Contents |
| :--- | :--- | :--- |
| RF link & protocol | [rf_crypto.md](memory/rf_crypto.md) | 868.950 MHz CC1110/CC1101 RF link, AES-128 KDF, replay filter, packet framing |
| CC1110 & Door Sensor | [cc1110_door.md](memory/cc1110_door.md) | CC1110 clocks/PM2/flash/SDCC, `VerisureDoorSensor` pins & power |
| Camera CC1110 (`MD_CAM`) | [md_cam_cc1110.md](memory/md_cam_cc1110.md) | Camera board CC1110 pins, power, PIR/tamper, alarm loop, RF to gateway |
| Camera Blackfin (`MD_CAM`) | [md_cam_blackfin.md](memory/md_cam_blackfin.md) | Blackfin `ADSP-BF512`, `U3` boot flash, JTAG |
| Camera image pipeline (`MD_CAM`) | [md_cam_image.md](memory/md_cam_image.md) | `OV7955`, PLL/PPI, AEC, JPEG, delta, IR flash |
| Gateway (`868Receiver`) | [gateway.md](memory/gateway.md) | ESP32-S2 coexistence, FreeRTOS radio task, GuardedWebServer, OTA, MQTT/HA |
| Siren (`SIREN`) | [siren.md](memory/siren.md) | MSP430F149, CC1101 26 MHz, `OKI L22901-707`, LPM3, `TH1`, BSL v1.10 |
| Smoke Detector (`SMOKE_DETECTOR`) | [smoke.md](memory/smoke.md) | GW-SD3 EFR32FG14P231, SWD AAP, `W25Q16JL` pads, `IC81`/`IC301`/`CP92`, 868.950 MHz |
| Build, Toolchains & CI | [build_ci.md](memory/build_ci.md) | SDCC/PlatformIO/CI gotchas, scripts, repo |
| Bench Tools | [bench_tools.md](memory/bench_tools.md) | Otii Arc, PSU, BSL/JTAG flashers, logic analysers |
