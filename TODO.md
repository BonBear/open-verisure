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

- [ ] **Change the gateway's temporary portal password.** Set on 2026-11-19 so the new portal login could be tested (Claude gave it to you in the chat). Change it on the page (Web Portal Password field) or on the serial console: `portalpass <new password>`.
- [ ] **0. Change the master key before deployment.** The current key was in two documents on the public `origin/main` from 2026-10-22 (replaced with a placeholder in commit 0f7fabd, still in the git history). Not serious while the system is not in real use, but before it is: a new key (`python derive_key.py --generate-master`) in the gateway's `secrets.h`, new `config.h` files, and reflash the gateway, all sensors and the camera.
- [ ] **4. Night pictures without the flash (idea).** The factory firmware lengthens the frame (VTS, 0x380E/0x380F) in the dark. On 2026-11-18 a `D1` in the dark was black (scene mean ~4/255), so without this only the flash works at night. Needs a dark room; Claude can run the test over JTAG (`MD_CAM/blackfin/jtag/bf_set.sh 0xFF800034 ...`) once the room is dark.
- [ ] **6. Update the pictures in the READMEs.** Replace outdated photos and screenshots in `README.md` and the component READMEs (`MD_CAM/README.md`, `VerisureDoorSensor/README.md`, `868Receiver/README.md`) with current ones, e.g. a camera picture from the gateway and the bench setup. Needs your choice of pictures: a camera picture shows your home and the repository is public.
- [ ] **8. Finish the review fixes (2026-11-17).** Committed but not fully verified: (a) camera tamper backoff (reports 2 s apart, doubling to 5 min) and at most one tamper picture per 10 min: test with the alarm OFF for the backoff (PIR motion pictures get in the way when armed) and armed for the picture limit. (b) was done on 2026-11-19: the door sensor flashed, its boot packet (block 53) accepted, events authenticated (`aes_wait`), heartbeats 60.0 min apart.
- [ ] **9. Verify the setup on the Linux laptop.** The scripts and builds were checked in WSL2 on 2026-11-18 (README "Linux setup"); left for a native laptop: serial access through `dialout`, flashing over `/dev/ttyUSB0`/`ttyACM0`. Build and flash the gateway, a door sensor and the camera from the laptop with only the release package, and run `MD_CAM/test_camera_e2e.py` there.
- [ ] **10. Review `MD_CAM` docs for the 22 unreferenced images in `MD_CAM/docs/images/`.** Check whether any of them belong in `MD_CAM/README.md` or the `MD_CAM/docs/` notes (downscale with `tools/prepare_photos.py` if kept), and remove/archive the rest (see cleanup survey from 2026-11-22).
