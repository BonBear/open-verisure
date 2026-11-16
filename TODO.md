# TODO

Two lists: what Claude can do on its own, and what needs someone at the camera or a decision.
Remove an item when it is done; its result goes in the commit message, the docs and, for
lessons worth keeping, `.agents/memory.md`.

**Status, 2026-11-16 night:** the camera works on the bench (pictures in 5–8 s, 640x480 in ~12 s,
deltas, IR flash in the dark, resend also during alarm sequences, PIR wake-up, PM2 sleep at
~0.05 mA idle, e2e 12/12). Everything is on `main`. What is left for the camera is mostly tests
on site (list B). Bench routines (flashing, JTAG, HOLD) are in `MD_CAM/README.md`.

## A. Claude can do unattended

- [ ] **The door sensor does not sleep.** `VerisureDoorSensor/main.c` reads reed/tamper/shock every ms in a busy loop (no PM2, no interrupts), which limits battery life. Add PM2 with port interrupts (P1_7, P1_2, P0_1) and the sleep timer for the heartbeat, with a watchdog as in the camera. Needs a door sensor on the flasher for testing. Check its spare pins for external pull-ups before driving them low in idle (the camera lost ~1.2 mA that way).
- [ ] **Resend asks for whole runs.** A resend round asks for up to 16 chunks from the first missing one, including chunks the gateway already has, so with every 7th chunk lost a 640x480 picture needed 32 of the 40 allowed rounds (`RS_MAX_ROUNDS` in `868Receiver/src/image_receiver.cpp`). Asking only for missing chunks (a bitmap or several runs per request), or raising the limit, would make a bad link cope better.
- [ ] **Camera silent once after a picture command.** On 2026-11-16, right after flashing both the gateway and the camera, the camera sent nothing for 70 s after a D1 command (no picture, no polls). Not seen again in the following runs, and the Otii showed no supply cut-off then. If it comes back: record the current (Blackfin on or not, CC1110 awake or not) and the gateway log.

## B. Needs you

- [ ] **0. Change the master key before deployment.** The current key was in two documents on the public `origin/main` from 2026-10-22 (replaced with a placeholder in commit 75062aa, still in the git history). Not serious while the system is not in real use, but before it is: a new key (`python derive_key.py --generate-master`) in the gateway's `secrets.h`, new `config.h` files, and reflash the gateway, all sensors and the camera.
- [ ] **3. Alarm in the dark with the IR flash.** Darken the room and switch off the other camera's IR light; alarm on and motion should give grayscale flash pictures (scene mean below `DARK_Y` 40). Light on: the same motion should give colour pictures and deltas.
- [ ] **4. Night pictures without the flash (idea).** The factory firmware lengthens the frame (VTS, 0x380E/0x380F) in the dark. Needs a dark room; Claude can run the test over JTAG (`MD_CAM/blackfin/jtag/bf_set.sh 0xFF800034 ...`) once the room is dark.
- [ ] **5. Range in the real location.** Put the camera where it will be mounted; a few D1/D2 show missing chunks per picture ("x/y chunks missing"), resend rounds and RSSI in the gateway log.
  - 2026-11-16, first try: gateway in the kitchen (two walls from the bench camera) received nothing at all (0 packets; noise floor -104 dBm). Objects near the camera's antenna may have made it worse, so this is not a clean limit.
