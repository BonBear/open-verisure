# TODO

Two lists: what Claude can do on its own, and what needs someone at the camera or a decision.
Remove an item when it is done; its result goes in the commit message, the docs and, for
lessons worth keeping, `.agents/memory.md`.

## A. Claude can do unattended

Nothing open.

## B. Needs you

- [ ] **0. Change the master key before deployment.** The current key was in two documents on the public `origin/main` from 2026-10-22 (replaced with a placeholder in commit 0f7fabd, still in the git history). Not serious while the system is not in real use, but before it is: a new key (`python derive_key.py --generate-master`) in the gateway's `secrets.h`, new `config.h` files, and reflash the gateway, all sensors and the camera.
- [ ] **4. Night pictures without the flash (idea).** The factory firmware lengthens the frame (VTS, 0x380E/0x380F) in the dark. On 2026-11-18 a `D1` in the dark was black (scene mean ~4/255), so without this only the flash works at night. Needs a dark room; Claude can run the test over JTAG (`MD_CAM/blackfin/jtag/bf_set.sh 0xFF800034 ...`) once the room is dark.
- [ ] **9. Verify the setup on the Linux laptop.** The scripts and builds were checked in WSL2 on 2026-11-18 (README "Linux setup"); left for a native laptop: serial access through `dialout`, flashing over `/dev/ttyUSB0`/`ttyACM0`. Build and flash the gateway, a door sensor and the camera from the laptop with only the release package, and run `MD_CAM/test_camera_e2e.py` there.
