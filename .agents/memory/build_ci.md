# Memory: Build, Toolchains & CI

## 🛠️ Build, Toolchains, Scripts & Repo
- [2026-11-18] [BUILD/PLATFORM_PIN]: Pin the PlatformIO platform and libraries: an unpinned platform fetched a new major version with a different, untested image. The gateway moved from espressif32@7.0.1 to 8.0.0 (Arduino core 3.x) with SmartRC-CC1101 3.0.2 pinned (GATEWAY/CORE3_SPI).
- [2026-11-19] [BUILD/SDCC_VERSIONS]: SDCC 4.5.0 (Ubuntu) and 4.6.0 (Windows) give slightly different CC1110 binaries (a few bytes); a seeded 4.5 camera build passed e2e 12/12. Both build without warnings and CI fails on any SDCC warning. In Git Bash SDCC is not in PATH: add "/c/Program Files/SDCC/bin".
- [2026-11-20] [BUILD/WINDOWS_ENV]: In the agent's Git Bash and PowerShell, `pio` is not on PATH either: add "$HOME/.platformio/penv/Scripts". The default `python`/`py` (3.12 in PATH, 3.15 via py) may lack `cryptography`, which the build scripts import even with --compile-only; use `py -3.12` (or 3.14), which has cryptography and pyserial.
- [2026-11-20] [REPO/RENAME]: The checkout moved from C:\repo\Antigravity to C:\repo\OpenVerisure. Tracked files use no absolute paths, but ignored local files still do: 868Receiver/.vscode (PlatformIO regenerates it), MD_CAM/blackfin/build/ov_scan.sh (hardcoded `cd`) and scratch/ scripts. "Antigravity" in .gitignore's .antigravity/ entry and in AGENTS.md is the agent tool, not the project.
- [2026-10-22] [Python-tools]: The serial port selector is centralized in `port_selector.py` and must be reused instead of hardcoding COM ports; the gateway is found on any OS by USB ID 303A:80C2 (`find_usb_port`).
- [2026-11-18] [SCRIPTS/LINUX]: SDCC 4.5 (Ubuntu) builds both CC1110 firmwares; pip on Linux installs paho-mqtt 2.x, which still runs the scripts' v1 callbacks (DeprecationWarning only).
- [2026-11-01] [TOOLS/BLACKFIN]: ADI GNU toolchain 2014R1 in C:\Tools\adi-blackfin\elf\bin (bfin-elf-gcc 4.3.5, bfin-elf-ldr, bfin-jtag = urJTAG with part bf518 and cables FT2232/J-Link/ICE-100B/USB-Blaster, bfin-gdbproxy). -mcpu=bf512-0.2 must be quoted in PowerShell; linker script via -Wl,-T. LDR: `bfin-elf-ldr -T BF518 -c -d 1`; MD_CAM/blackfin/build_ldr.py builds and checks the block headers.
- [2026-10-30] [MD_CAM/GIT_PRIVACY]: The repo's local git configuration uses the GitHub noreply address, and early unpushed commits were rewritten with --reset-author.

