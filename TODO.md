# TODO

Two lists: what Claude can do on its own, and what needs someone at the camera or a decision.
Tick items with `[x]` and add a short result after the item.

**Status, 2026-11-16 evening:** the camera works on the bench (pictures in 5–8 s, 640x480 in ~12 s,
deltas, IR flash in the dark, resend, PM2 sleep at ~0.05 mA idle, e2e 12/12). Everything is on `main`. What is left
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
- [x] **Find the 1.19 mA idle floor.** Done 2026-11-16: idle floor 1.163 mA -> **3 µA**, mean 1.213 -> **0.052 mA** (Otii, 3.0 V; with 3x CR123A ~10 years on idle alone, so battery self-discharge and pictures now set the life). e2e 12/12.
  - Cause: P0_6 and P0_7 go to the PIR board, which pulls both up to VBAT (5.8 and 3.7 kOhm, measured), and the firmware held them low while idle, sinking ~0.9 mA. They are now driven high while the Blackfin is off (`bf_power_off`; `bf_power_on` drives P0_7 low first, so the start-up sequence is unchanged). Driven high and tri-state gave the same floor; LD1 stays off and the PIR still triggers.
  - Also P1_2 (link enable) low while off: ~30 µA. P0_4/P0_5 tri-state and P1_3..P1_5 low made no difference; the IR flash board draws nothing in idle.
  - Method: test builds (`-DSLEEP_PIN_TEST=1`, removed again) changed the pin state in PM2 every ~18 s while the Otii recorded; the PIR header was mapped with a DMM (`MD_CAM/docs/Measurements_PIR_header.md`).
  - The last ~0.30 mA was P2_0: also pulled up (the pinout table said "not connected"), held low it sank 0.30 mA; now driven high while the Blackfin is off as well (floor 0.30 mA -> 3 µA). P0_1, P0_3, P1_0 and P1_1 tri-state made no difference; P0_0 and P0_2 (Blackfin rail enables) were not tried.
  - What is left: the 0.455 s wake-ups (~22 µA) and the polls every 27 s (~27 µA). Slower wake-ups would only matter now that they are half of the idle current.
- [ ] **Resend during an alarm sequence fails.** In the PIR test (B2, 2026-11-16) the first alarm picture had missing chunks: the gateway logged "Resend for 2NCEKS3T given up (no answer)" at 23:20:13 (right when the Blackfin was switched off), and its next request ("resend 16 chunks from 1") went out 14 s after the Blackfin was off, so the picture stayed broken on MQTT and `/api/camera.jpg`. With D1/D3 (e2e, `drop <n>`) resend works. Suspects: while armed with motion the camera takes the next picture (640x480) before the resend of the first is done, so the buffer no longer holds it; and `BF_KEEP_ON_S` runs out while a resend is still pending. Reproduce on the bench with the gateway's `drop <n>`, alarm on and motion; also why chunks went missing at -65 dBm (cardboard box and objects near the camera's antenna).
- [x] **Resend missing picture chunks.** Done 2026-11-16: the gateway asks for missing chunks (downlink 0xF0–0xFF, up to 16 chunks) with the answer to the poll after the picture; the camera sends them as 0x83 from the Blackfin's buffer (snap_link 0xE1). Tested with the gateway's `drop <n>`: every 7th dropped -> complete after 10 rounds / 3.2 s (640x480: 23 rounds / 7.6 s, also with the JPEG header lost), every 3rd -> 13 rounds / 4.1 s. The benefit on site shows in the range test (B5).

## B. Needs you

- [ ] **0. Change the master key before deployment.** The current key was in two documents on the public `origin/main` from 2026-10-22 (replaced with a placeholder in commit 75062aa, still in the git history). Not serious while the system is not in real use, but before it is: a new key (`python derive_key.py --generate-master`) in the gateway's `secrets.h`, new `config.h` files, and reflash the gateway, all sensors and the camera.

- [x] **1. Idle current with the Otii.** Done 2026-11-16: Otii Arc as the 3.0 V supply, 4 kHz recording (read back as min/max/mean per 5 ms):
  - Idle, alarm off (180 s): mean **1.244 mA**; alarm on (120 s): 1.239 mA. Of that, the floor is **1.193 mA (96 %)**; the wake-up every 0.455 s (~10 ms, peak ~4.6 mA, ~12 µAs) adds ~26 µA; the poll every 27.3 s (~35 ms, peak ~45 mA, ~0.72 mAs) adds ~26 µA. That is ~30 mAh/day.
  - One D1 picture: Blackfin on 21.1 s, 3.61 As (~1 mAh): ~7 s capture and send at 180–192 mA (~1.25 As), then 14 s at 170 mA for `BF_KEEP_ON_S` (~2.4 As). The Blackfin draws ~0.17–0.19 A at 3.0 V, not 0.14 A. Inrush at power-on peaks at ~0.78 A.
  - Decision: `BF_POLL_S` stays at 30 s (60 s would save only ~13 µA, 1 %). `BF_KEEP_ON_S` stays at 15 s for now: one picture event costs about as much as 50 min of idle, so the floor matters far more unless there are many alarm events a day. A first run the same day (Otii monitor values only) gave the same means.
- [x] **2. PIR wakes the camera from sleep.** Done 2026-11-16 (camera on the Otii, gateway next to it):
  - No false triggers: 5.5 min with nobody in the room and 72 s under a cardboard box without a single ALARM edge (seen in the current trace: ALARM low adds ~0.15 mA through the P1_7 pull-up). Tape over the lens is not enough to blind it.
  - Alarm off: lifting the box gave B9 and `motion=ON` at once, before the next regular poll (woken from PM2). `motion=OFF` came 30 s after the last edge.
  - Alarm on: the poll that delivered the armed state (motion still on) was followed by an alarm picture 7 s later (320x240, 8388 bytes), then a 640x480 one (25624 bytes) 18 s later. Blackfin on 44 s in all.
  - But the first alarm picture arrived with missing chunks that were never filled in (see the new item in list A).
- [ ] **3. Alarm in the dark with the IR flash.** Darken the room and switch off the other camera's IR light; alarm on and motion should give grayscale flash pictures (scene mean below `DARK_Y` 40). Light on: the same motion should give colour pictures and deltas.
- [ ] **4. Night pictures without the flash (idea).** The factory firmware lengthens the frame (VTS, 0x380E/0x380F) in the dark. Needs a dark room; Claude can run the test over JTAG (`MD_CAM/blackfin/jtag/bf_set.sh 0xFF800034 ...`) once the room is dark.
- [ ] **5. Range in the real location.** Put the camera where it will be mounted; a few D1/D2 show missing chunks per picture ("x/y chunks missing"), resend rounds and RSSI in the gateway log.
  - 2026-11-16, first try: gateway in the kitchen (two walls from the bench camera) received nothing at all (0 packets; noise floor -104 dBm). Objects near the camera's antenna may have made it worse, so this is not a clean limit.
- [x] **6. Review and merge the branch.** `md-cam/u3-flash-dump` was merged into `main` (fast-forward) and pushed on 2026-11-16.
- [x] **7. Test tools in the CC1110 firmware.** Removed on 2026-11-16 after your OK: main.c 2924 -> ~1000 lines, firmware 22490 -> 5989 bytes, e2e 11/11. Still in the git history.
