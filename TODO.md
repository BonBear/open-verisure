# TODO

Two lists: what Claude can do on its own, and what needs someone at the camera or a decision.
Remove an item when it is done; its result goes in the commit message, the docs and, for
lessons worth keeping, `.agents/memory.md`.

**Status, 2026-11-18:** a 7 h armed overnight run in the dark and a 7.8 h armed day run were clean (no silent poll gaps, no resets, every picture complete; armed idle 0.054 mA; e2e 12/12 in daylight).

**Status, 2026-11-16 night:** the camera works on the bench (pictures in 5–8 s, 640x480 in ~12 s,
deltas, IR flash in the dark, resend also during alarm sequences, PIR wake-up, PM2 sleep at
~0.05 mA idle, e2e 12/12). Everything is on `main`. What is left for the camera is mostly tests
on site (list B). Bench routines (flashing, JTAG, HOLD) are in `MD_CAM/README.md`.

## A. Claude can do unattended

Nothing open. (2026-11-22: the one-off camera silence and USB serial stall were dropped, not seen in many firmware iterations since; `v1.0.0` released.)

## B. Needs you

- [ ] **0. Change the master key before deployment.** The current key was in two documents on the public `origin/main` from 2026-10-22 (replaced with a placeholder in commit 0f7fabd, still in the git history). Not serious while the system is not in real use, but before it is: a new key (`python derive_key.py --generate-master`) in the gateway's `secrets.h`, new `config.h` files, and reflash the gateway, all sensors and the camera.
- [ ] **4. Night pictures without the flash (idea).** The factory firmware lengthens the frame (VTS, 0x380E/0x380F) in the dark. On 2026-11-18 a `D1` in the dark was black (scene mean ~4/255), so without this only the flash works at night. Needs a dark room; Claude can run the test over JTAG (`MD_CAM/blackfin/jtag/bf_set.sh 0xFF800034 ...`) once the room is dark.
- [ ] **9. Verify the setup on the Linux laptop.** The scripts and builds were checked in WSL2 on 2026-11-18 (README "Linux setup"); left for a native laptop: serial access through `dialout`, flashing over `/dev/ttyUSB0`/`ttyACM0`. Build and flash the gateway, a door sensor and the camera from the laptop with only the release package, and run `MD_CAM/test_camera_e2e.py` there.
