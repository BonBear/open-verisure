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

## B. Needs you

- [ ] **0. Change the master key before deployment.** The current key was in two documents on the public `origin/main` from 2026-10-22 (replaced with a placeholder in commit 0f7fabd, still in the git history). Not serious while the system is not in real use, but before it is: a new key (`python derive_key.py --generate-master`) in the gateway's `secrets.h`, new `config.h` files, and reflash the gateway, all sensors and the camera.
- [ ] **3. Alarm in the dark with the IR flash.** Darken the room and switch off the other camera's IR light; alarm on and motion should give grayscale flash pictures (scene mean below `DARK_Y` 40). Light on: the same motion should give colour pictures and deltas.
- [ ] **4. Night pictures without the flash (idea).** The factory firmware lengthens the frame (VTS, 0x380E/0x380F) in the dark. Needs a dark room; Claude can run the test over JTAG (`MD_CAM/blackfin/jtag/bf_set.sh 0xFF800034 ...`) once the room is dark.
- [ ] **5. Range in the real location.** Put the camera where it will be mounted; a few D1/D2 show missing chunks per picture ("x/y chunks missing"), resend rounds and RSSI in the gateway log.
  - 2026-11-17, camera on batteries at its spot (polls -64..-85 dBm): the gateway's CC1101 was ~60 kHz off (fixed, FSCTRL0) and now gets every poll. Pictures still lose 34-59 % of their 56-byte chunks, and while the Blackfin is on the camera receives no poll answers (its state keeps bit 0x04, no resend happens); with the Blackfin off it does (picture commands arrive). Likely the Blackfin's regulator desensitizes the CC1110 receiver, or the downlink is just weak. Next: a proper 868 MHz antenna on the gateway, then retest; options in firmware: send each poll answer twice, poll with the Blackfin off after a picture (it holds the picture), longer camera RX window.
  - 2026-11-16, first try: gateway in the kitchen (two walls from the bench camera) received nothing at all (0 packets; noise floor -104 dBm). Objects near the camera's antenna may have made it worse, so this is not a clean limit.
- [ ] **6. Update the pictures in the READMEs.** Replace outdated photos and screenshots in `README.md` and the component READMEs (`MD_CAM/README.md`, `VerisureDoorSensor/README.md`, `868Receiver/README.md`) with current ones, e.g. a camera picture from the gateway and the bench setup. Needs your choice of pictures: a camera picture shows your home and the repository is public.
- [ ] **7. Door sensor events.** PM2 sleep, the persistent counter (seeded from the gateway when flashing), the boot battery guard and the TX-storm fix are in `VerisureDoorSensor/main.c` and `VerisureDoorSensor/build_and_flash.py` (uncommitted), idle 17 uA, heartbeat and boot verified. Still to test with someone at the sensor: reed open/close, shock and tamper (lid on and off), their latency, and idle current with the lid on; then commit both.
