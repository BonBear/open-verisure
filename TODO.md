# TODO

Two lists: what Claude can do on its own, and what needs someone at the camera or a decision.
Remove an item when it is done; its result goes in the commit message, the docs and, for
lessons worth keeping, `.agents/memory.md`.

**Status, 2026-11-16 night:** the camera works on the bench (pictures in 5–8 s, 640x480 in ~12 s,
deltas, IR flash in the dark, resend also during alarm sequences, PIR wake-up, PM2 sleep at
~0.05 mA idle, e2e 12/12). Everything is on `main`. What is left for the camera is mostly tests
on site (list B). Bench routines (flashing, JTAG, HOLD) are in `MD_CAM/README.md`.

## A. Claude can do unattended

- [ ] **Camera silent once after a picture command.** On 2026-11-16, right after flashing both the gateway and the camera, the camera sent nothing for 70 s after a D1 command (no picture, no polls). Not seen again in the following runs, and the Otii showed no supply cut-off then. If it comes back: record the current (Blackfin on or not, CC1110 awake or not) and the gateway log.
- [ ] **Counter seed from the device's own log (review 2026-11-17, low).** The flash scripts seed the counter with the gateway's last block + 2, but a device can have used more blocks than the gateway saw (boots, lost packets). Reading the old log (0x7800-0x7FFF) over the debug interface before the chip erase and seeding with max(gateway, device) + 1 would close that. Needs a flash-read in the ESP_CC_Flasher API.
- [ ] **Rate limit for a rattling reed (review 2026-11-17, low).** A door at the magnet threshold sends a packet per change (up to ~10 per second); a backoff like the camera's tamper backoff would cap it.

## B. Needs you

- [ ] **0. Change the master key before deployment.** The current key was in two documents on the public `origin/main` from 2026-10-22 (replaced with a placeholder in commit 0f7fabd, still in the git history). Not serious while the system is not in real use, but before it is: a new key (`python derive_key.py --generate-master`) in the gateway's `secrets.h`, new `config.h` files, and reflash the gateway, all sensors and the camera.
- [ ] **3. Alarm in the dark with the IR flash.** Darken the room and switch off the other camera's IR light; alarm on and motion should give grayscale flash pictures (scene mean below `DARK_Y` 40). Light on: the same motion should give colour pictures and deltas.
- [ ] **4. Night pictures without the flash (idea).** The factory firmware lengthens the frame (VTS, 0x380E/0x380F) in the dark. Needs a dark room; Claude can run the test over JTAG (`MD_CAM/blackfin/jtag/bf_set.sh 0xFF800034 ...`) once the room is dark.
- [ ] **6. Update the pictures in the READMEs.** Replace outdated photos and screenshots in `README.md` and the component READMEs (`MD_CAM/README.md`, `VerisureDoorSensor/README.md`, `868Receiver/README.md`) with current ones, e.g. a camera picture from the gateway and the bench setup. Needs your choice of pictures: a camera picture shows your home and the repository is public.
- [ ] **8. Finish the review fixes (2026-11-17).** Committed but not fully verified: (a) camera tamper backoff (reports 2 s apart, doubling to 5 min) and at most one tamper picture per 10 min: test with the alarm OFF for the backoff (PIR motion pictures get in the way when armed) and armed for the picture limit; (b) flash the door sensor (counter-log retry, boot skips one block, 30 s pause after a watchdog reset) and check its boot packet is accepted. Needs the flasher moved (camera J6 / door J2).
