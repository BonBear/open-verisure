# MD CAM: the protocol as it works now

Describes the chain **Blackfin (snap_link) → CC1110 → radio → gateway → MQTT** as implemented
(status 2026-11-16). The sources are `MD_CAM/blackfin/snap_link.c`, `MD_CAM/firmware/main.c` and
`868Receiver/src/` (main.cpp, crypto.cpp, downlink.*, image_receiver.*, mqtt_manager.cpp). The
older analysis documents in this folder describe the investigation that led here and may
contain assumptions that turned out wrong.

## 1. Blackfin ↔ CC1110 (software SPI)

The CC1110 is the master (bit-banged mode 0, P1_2 selects the Blackfin), snap_link is the slave
on port H. Every byte received decides the answer in the *next* transfer.

| Command | Meaning |
|---|---|
| `0xD0` | 320x240 grayscale JPEG (or the mode in the JTAG word 0xFF800048) |
| `0xD1` | 320x240 colour, full picture (new delta reference, COM id in the JPEG) |
| `0xD2` | 640x480 colour (no reference) |
| `0xD3` | 320x240 colour as a delta against the reference, or a full picture when needed |
| `0xD4` | 320x240 grayscale with the IR flash (PF10+PF12 for 5 ms on the picture's own frame) |
| `0xD5` | alarm picture: as 0xD3 with light, as 0xD4 when the scene mean is below `DARK_Y` (40) |
| `0xD6` | alarm picture: as 0xD2 with light, as 0xD4 in the dark |
| `0xC0`+q | raw quarter q (test) |
| `0xB0` | answer 0x5A when the picture is ready, else 0x00 |
| `0xA0` | reset the read pointer |
| `0xE5` | ping: answer 0xA5 (the CC1110 waits for it after power-on instead of a fixed wait) |
| `0xE1` h l | read the picture again from chunk (h << 8 \| l), 40 bytes per chunk after the length bytes (resend) |
| other | next picture byte (the CC1110 sends 0x00 while reading) |

After 0x5A the CC1110 reads the length (2 bytes, little endian) and then the data.

**Picture pipeline in snap_link:** a whole frame by one DMA into SDRAM; a 320x240 pixel is the
mean of 2x2 sensor pixels; radial lens shading correction; U/V saturation x7/4 after the white
balance; sharpening (only where an MCU is encoded); JPEG Q65. A delta uses the quality its full
picture had. Before each delta the colour drift against the reference is measured in MCUs whose
luma did not change and compensated, so only real changes are sent.

**Delta blob** (answer to 0xD3/0xD5 when a reference exists):
`'D' '1' id mcus_lo mcus_hi count_lo count_hi`, then per changed MCU
`idx_lo idx_hi len_lo len_hi` + `len` bytes of entropy-coded data (one restart interval).
An MCU counts as changed when any 8x8 block (Y, U or V) has a mean |difference| above 12
(`DELTA_T`); its neighbours count from half that threshold. More than half changed gives a full
picture.

**Tuning words over JTAG** (`MD_CAM/blackfin/jtag/bf_set.sh`): 0xFF800030 sharpening,
0xFF800034 sensor register, 0xFF80003C lens shading/saturation, 0xFF800040 JPEG quality,
0xFF800050 delta threshold, 0xFF800078 flash test. Status words and the delta histogram are read
with `bf_read.sh`.

## 2. Radio uplink (camera → gateway), 868.950 MHz, 38.4 kBaud 2-FSK

Every packet starts with the device ID in plain text (8 ASCII characters), followed by AES-128
with the device key (derived from the master key). The radio adds a CRC-16.

**Short packet, 24 bytes** (telemetry, poll, events): ID + one AES block (ECB).
Plain text: `ctr_lo(2) flags batt% mv(2) id[0..3] 'V' 'S' type arg ctr_hi(2)`. The 32-bit packet
counter (bytes 0..1 big endian and 14..15 little endian, flag 0x20) is persistent: its upper
24 bits are logged in the CC1110's last two flash pages (0x7800–0x7FFF) before they are used,
at every boot and every 256 packets, as in the door sensor, so it never goes back, not even
after a reset ([cryptography_and_protocol.md](../../documentation/cryptography_and_protocol.md)).
Flags: 0x01 motion, 0x04 tamper (SW3 released, camera off the wall), 0x08 power-on (first packet
after a power-on reset), 0x10 low battery, 0x20 counter format, bits 7:6 reset cause. Byte 12 is
the type:

| type | Meaning |
|---|---|
| `0x00` | announce, the first packet after a boot (counter low byte 0) |
| `0xB2` | picture start, sent before every picture: arg = picture command; its counter tags the picture's chunks |
| `0xBA` | poll: arg = state (0x01 armed, 0x02 Blackfin on, 0x04 acknowledgement, bits 7:3 battery under load: 0 unknown, else 2275 + 25 × value mV) |
| `0xB9` | motion: arg = 1 start, 0 end |
| `0xB5` | tamper changed: arg = 1 tamper (SW3 released), 0 secure |
| `0xB8` | picture failed (no answer from the Blackfin within ~50 s): arg = its last status |
| `0xB3` | picture failed (implausible length): arg = length high byte |
| `0xB7`, `0xB4`, `0xB6` | test (only with `BF_IMG_TIMING 1`): time per picture packet for the RF send and the SPI read, boot pings |

**Tamper:** SW3 closes P1_6 to VBAT while the camera is pressed against the wall. Port 1 keeps
its pull-up (the PIR on P1_7 needs it), so every 0.5 s tick the camera switches the port to
pull-down for ~45 µs, with P1_7 tri-stated, and reads P1_6: 0 = released = tamper.

**Battery:** the CR123A cells hold their idle voltage until near the end, so the camera also
measures VBAT while the Blackfin is powered (~0.1–0.2 A) and keeps the lowest value of the last
time it was on. Battery % is the lower of two linear estimates: idle 2600–3000 mV and under load
2450–2850 mV (pictures stop at ~2.4 V, Otii 2026-11-17). Low-battery flag: idle below 2700 mV or
under load below 2600 mV. The value under load goes out in every poll (Home Assistant: "Battery
Voltage Under Load", unknown until the first picture after a restart of the gateway or the camera).
An idle reading right after PM2 waits ~11 ms first: without it the ADC read ~110 mV low.

Below 2.3 V at boot the camera neither logs a counter block nor sends; it sleeps and measures
again about every minute. The build script reads the counter seed from the gateway and
stops if it cannot (`--no-seed` flashes anyway; then `replay reset <ID>` on the gateway).

**Picture packet, 56 bytes, type 0x82:** ID + three AES blocks chained as CBC with a zero IV
(C0 = E(P0), C1 = E(P1 ⊕ C0), C2 = E(P2 ⊕ C1)). Plain text (48 bytes):
`tag_lo(2) 0x82 chunk(2) flags data[40] tag_hi(2)`, flags: 0x80 first, 0x40 last, bits 0–5
length. The tag (big endian) is the counter of the picture's start packet (0xB2). The gateway
rebuilds it from the camera's last accepted counter and accepts a chunk only if it is at most
128 before or 64 after that counter and its high half matches, and a chunk must belong to the
newest picture seen or a newer one (a resent chunk to exactly the newest), so a recorded picture
is dropped (`[Replay] Dropped picture chunks`); a replay of the newest picture only repeats it. Chunk n carries bytes 40n.. of the picture. About 20 ms per packet: a 320x240 picture of ~7 KB
takes ~4 s on air, 5–8 s from the command to the finished picture.
**Resent chunk, 56 bytes, type 0x83:** the same format as 0x82 with the same tag, sent only on the
gateway's request. (Older picture packets, type 0x80, 24 bytes with 8 bytes of data and no counter,
are no longer accepted since 2026-11-17.) Bench test: `-DPIC_TAG_SKEW=300` builds a camera whose
chunks carry an older tag, as a recorded picture would; the gateway must drop them.

## 3. Reception in the gateway

The CC1101 receives in fixed length, **58 bytes with the hardware CRC off** (PKTCTRL0 0x00), plus
two status bytes. The software checks the CRC (CC1101 CRC-16, polynomial 0x8005, start 0xFFFF):

1. the CRC matches after 56 bytes and the packet decodes as type 0x82/0x83 → picture chunk;
2. otherwise the CRC matches after 24 bytes → normal sensor or camera packet;
3. otherwise the frame is dropped.

After a 24-byte packet the radio listens to noise for another ~7 ms; a packet that starts then is
missed. The 64-byte FIFO is read in pieces while the frame comes in; the last byte is left until
the frame is complete. The radio stays in RX after a packet (the FIFO is not flushed). The
downlink is sent with PKTLEN 24 and the CRC on, then back to 58.

Picture chunks are placed by their number; missing chunks stay zero. A picture ends with its last
chunk, the first chunk of the next picture, a poll from the camera, or 5 s of silence ("end lost",
an EOI is added). **Resend:** if a full picture (0x82) has missing chunks it is published anyway,
and with the answer to the camera's next poll (which comes right after the picture) the gateway
asks for the missing chunks among the 32 from the first missing one (a bitmap, so chunks it
already has are not sent again); the camera sends them as 0x83 and polls again, until the
picture is complete (at most 40 rounds / 60 s). On the bench with every 7th chunk dropped, a
640x480 picture (70 of 493 chunks missing) was complete after 14 rounds in 2.2 s (32 rounds
with the earlier runs of 16). The complete picture is then
stored and published as usual. A full 320x240 picture with a COM id becomes the base for deltas;
grayscale pictures without an id are kept apart (the web page shows the newest); 640x480 pictures
are kept apart.

## 4. Radio downlink (gateway → camera)

An answer to every poll within ~0.1 s, 24 bytes: ID + an AES block with the plain text
`'D' 'L' ctr(4) armed cmd first(2) map(4) 'V' 'S'`. `ctr` is the poll's 32-bit counter (big
endian); the camera accepts an answer only if it is the counter of its own last poll, and since
the counter never repeats, a recorded answer never fits again. (Until 2026-11-17 the answer
carried the poll's 16-bit seq and a 16-bit challenge.) The gateway answers only polls in the
counter format. `cmd`: 0 = none, 0xD1–0xD6 = take a picture, 0xE8 = HOLD (service: Blackfin on
for ~10 min without a picture, for JTAG flashing; MQTT `HOLD`), 0xF0 = resend: chunk `first + i`
for every bit i set in `map` (byte 10 bit 0 = i 0, little endian); `first` and `map` are 0
otherwise.

## 5. MQTT (base topic `verisure`)

| Topic | Direction | Content |
|---|---|---|
| `verisure/<id>/picture/set` | to the gateway | `D1`..`D6` (anything else: D2); `HOLD` (service) |
| `verisure/alarm/set` | to the gateway | `ON` / `OFF` (also alarm_control_panel states) |
| `verisure/<id>/image` | from the gateway | latest picture as JPEG |
| `verisure/<id>/state` | from the gateway | telemetry (JSON) |

Home Assistant gets the buttons "Take 640x480 picture" (D2) and "Take flash picture" (D4).

## 6. The camera's states (cam_alarm_loop)

- Blackfin off: the CC1110 sleeps in PM2 (0.5 s sleep timer or a PIR edge), watchdog ~1 s.
- Poll every ~30 s (`BF_POLL_S`) while armed, during motion or with the Blackfin on; every
  ~120 s (`BF_POLL_DISARMED_S`) when disarmed and idle; and right after every picture or resend.
  Motion always polls first, so it gets a fresh alarm state; a picture requested from Home
  Assistant while disarmed waits up to ~2 minutes for the next poll.
- Tamper: a change is sent at once (0xB5); every packet carries the state.
- Armed and motion: first picture 0xD5, second 0xD6, then 0xD5 every ~5 s while the motion lasts
  (`BF_PIR_HOLD_S` 30 s). Armed without motion: 0xD5 every 30 minutes.
- The Blackfin stays on for 15 s after a good picture (`BF_KEEP_ON_S`), so a delta can be taken
  against the reference in SDRAM; HOLD keeps it on for ~10 minutes.
