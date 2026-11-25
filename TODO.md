# TODO

Two lists: what Claude can do on its own, and what needs someone at the camera or a decision.
Remove an item when it is done; its result goes in the commit message, the docs and, for
lessons worth keeping, `.agents/memory.md`.

## A. Claude can do unattended

Nothing open.

## B. Needs you

- [ ] **0. Change the master key before deployment.** The current key was in two documents on the public `origin/main` from 2026-10-22 (replaced with a placeholder in commit 0f7fabd, still in the git history). Not serious while the system is not in real use, but before it is: a new key (`python derive_key.py --generate-master`) in the gateway's `secrets.h`, new `config.h` files, and reflash the gateway, all sensors and the camera.
- [ ] **9. Verify the setup on the Linux laptop.** The scripts and builds were checked in WSL2 on 2026-11-18 (README "Linux setup"); left for a native laptop: serial access through `dialout`, flashing over `/dev/ttyUSB0`/`ttyACM0`. Build and flash the gateway, a door sensor and the camera from the laptop with only the release package, and run `MD_CAM/test_camera_e2e.py` there.
- [ ] **10. Full-volume SIREN piezo horn test (`CN2` + `P1.7` HV boost).** Reconnect `CN2` (piezo horn, Saleae Logic 4 already disconnected), build `SIREN/firmware` with `-DSIREN_ENABLE_HV_BOOST` and flash via ROM BSL, then trigger `alarm`, `ding_dong`, `chime`, `double_beep`, and `beep` from Home Assistant when alone at home.
