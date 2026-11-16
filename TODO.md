# TODO

Two lists: what Claude can do on its own, and what needs someone at the camera or a decision.
Tick items with `[x]` and add a short result after the item.

**Status, 2026-11-16 evening:** the camera works on the bench (pictures in 5–8 s, 640x480 in ~12 s,
deltas, IR flash in the dark, resend, PM2 sleep, e2e 12/12). Everything is on `main`. What is left
for the camera is measurements and tests on site (list B). Left to do unattended: sleep for the
door sensor (needs a door sensor on the flasher). Bench routines (flashing, JTAG, HOLD) are in
`MD_CAM/README.md`.

## A. Claude can do unattended

- [x] **Show flash pictures on the gateway's web page.** Done 2026-11-16: `/api/camera.jpg` serves the newest of the colour picture and the flash picture (verified D1 -> colour, D4 -> grayscale, D1 -> colour).
- [x] **Extend the e2e test** (`MD_CAM/test_camera_e2e.py`). Done 2026-11-16: now checks 56-byte packets (packets = bytes / 40), D4 grayscale on MQTT, D3 after D4 as a delta, D5/D6 (colour with light, flash picture in the dark). 11/11 passed.
- [x] **Update the protocol documentation.** Done 2026-11-16: new document `MD_CAM/docs/Camera_protocol.md` (SPI commands 0xD0–0xD6, delta format, 24/56-byte radio packets, the gateway's 58-byte reception, downlink, MQTT, the camera's states), referenced from the old analysis.
- [x] **Software sharpening.** Done 2026-11-16: sharpening q4 (y + q/32·(4y − neighbours)) only where an MCU is encoded; the reference stays unsharpened, so deltas are not affected. Detail (Laplacian sd) 16.9 -> 22.6, file +7 %, noise on flat areas unchanged; q8 gave 27.6 but more noise. Applies to 320x240 colour pictures (full and delta). Tunable over JTAG at 0xFF800030. L1 code extended to 32 KB (probed over JTAG).
- [x] **Colour share of the delta noise.** Investigated 2026-11-16: with U/V weight 4/2/0 (0 = luma only) the noise floor stayed at 4–6; it is luma (probably lamp flicker between frames), not colour. No change, `DELTA_T` 12 is kept.
- [x] **Tidy the CC1110 firmware.** 2026-11-16: stale comments fixed (packet format 0x82, 640x480 picture ~12 s). The test tools behind switched-off flags were removed later (see B7).
- [x] **Optimisation flags for snap_link.** Done 2026-11-16: -O2, grab + encode 3–4x faster (320: 0.86 -> 0.29 s, 640: 2.61 -> 0.60 s).
- [x] **Shorten the Blackfin boot wait.** Done 2026-11-16: the fixed wait (~3 s) is replaced by a ping (0xE5 -> 0xA5) after ~140 ms; snap_link answers after ~1.2 s (37 pings). Cold picture 8 s -> 6–7 s from the command, ~2 s less Blackfin time per alarm event. e2e 11/11.
- [ ] **The door sensor does not sleep.** `VerisureDoorSensor/main.c` reads reed/tamper/shock every ms in a busy loop (no PM2, no interrupts), which limits battery life. Add PM2 with port interrupts (P1_7, P1_2, P0_1) and the sleep timer for the heartbeat, with a watchdog as in the camera. Needs a door sensor on the flasher for testing.
- [x] **Exposure after a cold start.** Done 2026-11-16: it was the colour, not the exposure (Cb +4..5 in the first seconds, luma unchanged; the sensor's white balance; 0x3406 did not help). snap_link now measures the colour shift against the reference in blocks whose luma did not change and compensates before the delta check: first delta after a cold start 118–134 blocks -> 0. e2e 12/12.
- [x] **Erase the factory firmware from the Blackfin boot flash.** Done 2026-11-16: all dumps copied to `C:\repo\verisure_firmware_dumps\` (with a SHA-256 list and a README), the copy at 0x10000 read back from the chip and identical to the dump, then erased (0x10000–0x17FFF, checked blank). The camera boots as before, e2e 12/12. The door sensor's factory firmware was never dumped.
- [x] **Resend missing picture chunks.** Done 2026-11-16: the gateway asks for missing chunks (downlink 0xF0–0xFF, up to 16 chunks) with the answer to the poll after the picture; the camera sends them as 0x83 from the Blackfin's buffer (snap_link 0xE1). Tested with the gateway's `drop <n>`: every 7th dropped -> complete after 10 rounds / 3.2 s (640x480: 23 rounds / 7.6 s, also with the JPEG header lost), every 3rd -> 13 rounds / 4.1 s. The benefit on site shows in the range test (B5).

## B. Needs you

- [ ] **0. Change the master key before deployment.** The current key was in two documents on the public `origin/main` from 2026-10-22 (replaced with a placeholder in commit 75062aa, still in the git history). Not serious while the system is not in real use, but before it is: a new key (`python derive_key.py --generate-master`) in the gateway's `secrets.h`, new `config.h` files, and reflash the gateway, all sensors and the camera.

- [ ] **1. Idle current with the Otii.** The firmware now sleeps in PM2 while the Blackfin is off; the bench supply only shows "below 1 mA".
  - Measure in series at 3.0 V for at least 2 min idle (alarm off): mean, floor, peaks for the poll (every ~28 s, transmit + ~110 ms listening) and the wake-up every 0.5 s.
  - The same with the alarm on, and one full picture (D1 via MQTT `verisure/2NCEKS3T/picture/set`): Blackfin ~0.14 A for ~5–8 s plus 15 s on afterwards (`BF_KEEP_ON_S`).
  - From the result: decide whether `BF_POLL_S` (30 s) and `BF_KEEP_ON_S` (lowered from 120 to 15 s on 2026-11-16 pending this measurement) should change. Note: 120 s on after every picture costs ~17 As (0.14 A), more than the picture itself; now that a full picture takes only ~5 s, a shorter time can be worth the occasional full picture instead of a delta.
- [ ] **2. PIR wakes the camera from sleep** (not tried with this build).
  - Alarm off: move in front of the camera; the gateway log should show B9 within a second or so.
  - Alarm on (`verisure/alarm/set` ON): motion should give an alarm picture (0xD5) within ~10 s.
- [ ] **3. Alarm in the dark with the IR flash.** Darken the room and switch off the other camera's IR light; alarm on and motion should give grayscale flash pictures (scene mean below `DARK_Y` 40). Light on: the same motion should give colour pictures and deltas.
- [ ] **4. Night pictures without the flash (idea).** The factory firmware lengthens the frame (VTS, 0x380E/0x380F) in the dark. Needs a dark room; Claude can run the test over JTAG (`MD_CAM/blackfin/jtag/bf_set.sh 0xFF800034 ...`) once the room is dark.
- [ ] **5. Range in the real location.** Put the camera where it will be mounted; a few D1/D2 show missing chunks per picture ("x/y chunks missing"), resend rounds and RSSI in the gateway log.
- [x] **6. Review and merge the branch.** `md-cam/u3-flash-dump` was merged into `main` (fast-forward) and pushed on 2026-11-16.
- [x] **7. Test tools in the CC1110 firmware.** Removed on 2026-11-16 after your OK: main.c 2924 -> ~1000 lines, firmware 22490 -> 5989 bytes, e2e 11/11. Still in the git history.
