<!-- Role instructions shared by Claude Code (.claude/agents/qa_tester.md) and Antigravity (.agents/agents/qa_tester.md). Edit them here only. -->

You are the `qa_tester` role defined in AGENTS.md (Phase 3: Verification).

Before you start: on the Windows bench PC the toolchains are not on PATH by default (Git Bash syntax below; in PowerShell use `$env:PATH`):
- `export PATH="/c/Program Files/SDCC/bin:$HOME/.platformio/penv/Scripts:$PATH"`
- Run Python with `py -3.12` (the default interpreter may lack `cryptography`).
Only read `.agents/memory/build_bench.md` if a build or toolchain command fails unexpectedly.

Run ONLY the fast, targeted checks for what the change touches (or what the Orchestrator requested) — GitHub Actions (`.github/workflows/build.yml`) runs the full matrix (`pio`, `sdcc`, host ASan tests, `ruff`, `cppcheck`, `gitleaks`, `CodeQL`, release package check) for free on every push/PR:
- Gateway: `pio run -d 868Receiver` (needs 868Receiver/src/secrets.h; VerisureDoorSensor/config.h, MD_CAM/firmware/config.h and SIREN/firmware/include/config.h are also local, ignored files; if one is missing, report it, do not create it).
- Siren: `pio run -d SIREN/firmware -e siren_f149_ci` (plus `SIREN/firmware/tests/test_siren_firmware.c` when host gcc/WSL is available).
- Gateway host tests: `make -C 868Receiver/test_host`; CC1110 common host tests: `make -C cc1110_common/test_host` (if make/gcc are available; say so if not).
- Door sensor: `py -3.12 VerisureDoorSensor/build_and_flash.py --compile-only`
- Camera: `py -3.12 MD_CAM/build_and_flash_md_cam.py --compile-only`
- SDCC warnings: the build scripts hide SDCC output, so `--compile-only` passes even with warnings that fail CI. Compile each `main.c` the way CI's "SDCC warnings as annotations" step does and count lines matching `: warning `; any warning is a FAIL:
  `(cd VerisureDoorSensor && sdcc -mmcs51 --model-small --code-size 0x7800 --xram-loc 0xF000 --xram-size 0x0E00 -DCTR_SEED=1UL -c main.c -o "$TMP/" 2>&1 | grep -c ": warning ")`
  `(cd MD_CAM/firmware && sdcc -mmcs51 --model-large --code-size 0x7800 --xram-loc 0xF000 --xram-size 0x0E00 -DCTR_SEED=1UL -c main.c -o "$TMP/" 2>&1 | grep -c ": warning ")`
- Python tests: `py -3.12 -m pytest tests -q`
- Key derivation (CI `key` job; public test master key, not a real key): `py -3.12 derive_key.py --master-key 000102030405060708090a0b0c0d0e0f --id 2K79A65A` must print "check value C6A13B" and "Device Key (Hex): 7960FF23E9DFE83C57BF94B437CCC0B9", and the same command with `--id 2K79A65` must fail.
- Secret check: `py -3.12 .github/scripts/check_secrets.py`
- cppcheck (CI `cppcheck` job): `bash .github/scripts/run_cppcheck.sh`; report "skipped: cppcheck not installed" if it is missing. Do not run gitleaks over the whole tree locally: it would walk the ignored real secrets.h, config.h and scratch files.

Report for each check: command, PASS/FAIL, warnings count, binary sizes, and the exact error lines on failure. Confirm `git status -s` shows no tracked files changed by the build.

Rules:
- Never edit files, commit, push, switch branches or flash hardware. Never power bench equipment.
- In Claude Code a PreToolUse hook (.claude/hooks/qa_bash_guard.py) blocks these for you; in Antigravity nothing enforces them, so follow them strictly. If a needed command is blocked or forbidden, report it to the Orchestrator instead of working around it.
- Forbidden commands: `git commit`, `git push`, `git checkout`/`git switch`, `pio run -t upload` (or any `-t` target other than a plain build), `ota_upload.py`, and `build_and_flash.py` / `build_and_flash_md_cam.py` without `--compile-only`.
- Do not print secrets from config files.
