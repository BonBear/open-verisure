#!/usr/bin/env python3
"""
test_tamper.py - Bench check for MD_CAM tamper backoff and armed tamper picture cooldown.

Needs: the camera powered and flashed, the gateway on USB (found by its USB ID, or --port).
Cover the PIR before running so motion does not trigger extra pictures. The script only reads
the gateway serial log and sends documented gateway CLI commands (alarm off / alarm on); no
flashing. Lines that could show a key (32 hex digits) are masked.

    python MD_CAM/test_tamper.py --port COM9
    python MD_CAM/test_tamper.py --log saved_serial.log

Checks (matching MD_CAM/firmware/main.c):
  1. With alarm OFF: tamper reports (P0=0xB5) start ~2 s apart and double per change up to
     the 5 min (300 s) cap (2, 4, 8, 16, 32, 64, 128, 256, 300 s).
  2. With alarm ON: tamper triggers a picture, and repeated tamper triggers produce at most
     one tamper picture per 10 min (600 s).
Exit code 0 when every selected check passed.
"""
import argparse
from dataclasses import dataclass
import os
import re
import sys
import threading
import time
from typing import Iterable, Optional, Sequence, Tuple, Union

try:
    import serial
except ImportError:  # pragma: no cover - host tests only need pure log functions
    serial = None

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_H = os.path.join(HERE, "firmware", "config.h")
HEX32 = re.compile(r"\b[0-9A-Fa-f]{32}\b")

TAMPER_MIN_GAP_S = 2.0
TAMPER_MAX_GAP_S = 300.0
TAMPER_CALM_S = 600.0
TAMPER_PIC_GAP_S = 600.0
DEFAULT_BACKOFF_REPORTS = 10   # 9 gaps: 2, 4, 8, 16, 32, 64, 128, 256, 300 s
DELIVER_WAIT_S = 150.0         # disarmed polls are 120 s apart, plus margin

TS_RX = re.compile(r"^\[\s*(\d+(?:\.\d+)?)\s+s\]")
AUTH_RX = re.compile(r'\[AUTHENTICATED\]\s+Device:\s*"([A-Za-z0-9]{1,8})"\s*\|\s*Seq:\s*#(\d+)')
PINS_RX = re.compile(r"^\s*Pins\s*:\s*P0=0x([0-9A-Fa-f]{2})\s+P1=0x([0-9A-Fa-f]{2})\b")
DOWNLINK_ARMED_RX = re.compile(r"\[Downlink\]\s*->\s*([A-Za-z0-9]{1,8}):\s*armed\s+([01])\b")
IMG_RX = re.compile(r"\[ImageReceiver\]\s+(JPEG|Delta)\s+from\s+([A-Za-z0-9]{1,8}):")
RESEND_DONE_RX = re.compile(r"\[ImageReceiver\]\s+Picture\s+from\s+([A-Za-z0-9]{1,8})\s+complete\s+after\s+resend:")


@dataclass(frozen=True)
class TamperEvent:
    t: float
    device: str
    seq: int
    active: bool


@dataclass(frozen=True)
class PictureEvent:
    t: float
    device: str
    kind: str                  # "start" (0xB2), "jpeg", or "delta"
    cmd: Optional[int]
    seq: Optional[int]


@dataclass(frozen=True)
class MotionEvent:
    t: float
    device: str
    active: bool
    seq: int


@dataclass(frozen=True)
class ArmedEvent:
    t: float
    device: str
    armed: bool


@dataclass(frozen=True)
class BackoffCheckResult:
    ok: bool
    detail: str
    gaps: list
    expected_gaps: list


@dataclass(frozen=True)
class ArmedPicCheckResult:
    ok: bool
    detail: str
    trigger_times: list
    picture_times: list
    picture_gaps: list


@dataclass(frozen=True)
class CalmCheckResult:
    ok: bool
    detail: str
    gap: Optional[float]


@dataclass(frozen=True)
class TamperLogAnalysis:
    backoff: BackoffCheckResult
    calm: Optional[CalmCheckResult]
    armed: ArmedPicCheckResult


Event = Union[TamperEvent, PictureEvent, MotionEvent, ArmedEvent]


def expected_backoff_sequence(count: int, min_gap_s: float = TAMPER_MIN_GAP_S,
                              max_gap_s: float = TAMPER_MAX_GAP_S) -> list:
    """Return `count` consecutive tamper backoff gaps matching main.c's doubling rule."""
    gaps = []
    cur = float(min_gap_s)
    for _ in range(max(0, count)):
        gaps.append(cur)
        cur = cur * 2.0 if cur < max_gap_s / 2.0 else float(max_gap_s)
    return gaps


def parse_gateway_log(source: Union[str, Iterable[Union[str, Tuple[float, str]]]],
                      device_id: Optional[str] = None) -> list:
    """Parse gateway serial text or (timestamp, line) pairs into typed camera/downlink events."""
    if isinstance(source, str):
        items = source.splitlines()
    else:
        items = list(source)

    events: list = []
    last_t = 0.0
    cur_pkt = None
    resend_done_at = {}

    for item in items:
        if isinstance(item, tuple):
            line_t = float(item[0])
            line = item[1]
            last_t = line_t
        else:
            line = item
            ts_m = TS_RX.match(line)
            if ts_m:
                last_t = float(ts_m.group(1))
            line_t = last_t

        auth_m = AUTH_RX.search(line)
        if auth_m:
            cur_pkt = {
                "t": line_t,
                "device": auth_m.group(1),
                "seq": int(auth_m.group(2)),
            }
            continue

        if cur_pkt is not None:
            pins_m = PINS_RX.match(line)
            if pins_m:
                p0 = int(pins_m.group(1), 16)
                p1 = int(pins_m.group(2), 16)
                dev = cur_pkt["device"]
                if device_id is None or dev == device_id:
                    if p0 == 0xB5:
                        events.append(TamperEvent(cur_pkt["t"], dev, cur_pkt["seq"], p1 != 0))
                    elif p0 == 0xB2:
                        events.append(PictureEvent(cur_pkt["t"], dev, "start", p1, cur_pkt["seq"]))
                    elif p0 == 0xB9:
                        events.append(MotionEvent(cur_pkt["t"], dev, p1 != 0, cur_pkt["seq"]))
                cur_pkt = None
                continue
            if line.strip() and not line.startswith((" ", "\t")):
                cur_pkt = None

        dl_m = DOWNLINK_ARMED_RX.search(line)
        if dl_m:
            dev = dl_m.group(1)
            if device_id is None or dev == device_id:
                events.append(ArmedEvent(line_t, dev, dl_m.group(2) == "1"))
            continue

        rs_m = RESEND_DONE_RX.search(line)
        if rs_m:
            resend_done_at[rs_m.group(1)] = line_t
            continue

        img_m = IMG_RX.search(line)
        if img_m:
            kind = img_m.group(1).lower()
            dev = img_m.group(2)
            if device_id is None or dev == device_id:
                rs_t = resend_done_at.pop(dev, None)
                if rs_t is not None and 0.0 <= line_t - rs_t <= 5.0:
                    continue  # re-published JPEG from the same picture after resend
                events.append(PictureEvent(line_t, dev, kind, None, None))

    return events


def collect_pictures(events: Sequence[Event]) -> list:
    """Collapse each (0xB2 start -> ImageReceiver JPEG/Delta) pair into one PictureEvent."""
    pics = []
    open_start = {}
    for ev in events:
        if not isinstance(ev, PictureEvent):
            continue
        if ev.kind == "start":
            pics.append(ev)
            open_start[ev.device] = ev
        else:
            prev = open_start.get(ev.device)
            if prev is not None and 0.0 <= ev.t - prev.t <= 120.0:
                open_start[ev.device] = None
            else:
                pics.append(ev)
                open_start[ev.device] = None
    return pics


def split_events_by_armed(events: Sequence[Event], initial_armed: bool = False) -> Tuple[list, list]:
    """Partition events into (disarmed_events, armed_events) based on ArmedEvent transitions."""
    disarmed = []
    armed_evs = []
    armed = initial_armed
    for ev in events:
        if isinstance(ev, ArmedEvent):
            armed = ev.armed
            continue
        if armed:
            armed_evs.append(ev)
        else:
            disarmed.append(ev)
    return disarmed, armed_evs


def check_tamper_backoff(
    report_times: Sequence[float],
    min_gap_s: float = TAMPER_MIN_GAP_S,
    max_gap_s: float = TAMPER_MAX_GAP_S,
    jitter_s: float = 0.4,
    start_max_s: float = 6.0,
    cap_slack_s: float = 30.0,
    require_cap: bool = True,
) -> BackoffCheckResult:
    """Check that 0xB5 tamper report times start at ~2 s and double up to the 5 min (300 s) cap."""
    times = list(report_times)
    if len(times) < 2:
        return BackoffCheckResult(False, f"need >= 2 tamper reports, got {len(times)}", [], [])

    gaps = [times[i] - times[i - 1] for i in range(1, len(times))]
    expected = expected_backoff_sequence(len(gaps), min_gap_s=min_gap_s, max_gap_s=max_gap_s)
    reached_cap = False

    for idx, (gap, exp) in enumerate(zip(gaps, expected), start=1):
        if gap < exp - jitter_s:
            return BackoffCheckResult(
                False,
                f"gap #{idx} was {gap:.2f} s (< expected {exp:.0f} s - {jitter_s:.1f} s)",
                gaps,
                expected,
            )
        if idx == 1 and gap > start_max_s:
            return BackoffCheckResult(
                False,
                f"first gap was {gap:.2f} s (> {start_max_s:.1f} s; did not start near {min_gap_s:.0f} s)",
                gaps,
                expected,
            )
        if exp >= max_gap_s:
            reached_cap = True
            if gap > max_gap_s + cap_slack_s:
                return BackoffCheckResult(
                    False,
                    f"gap #{idx} at cap was {gap:.2f} s (> {max_gap_s:.0f} + {cap_slack_s:.0f} s; missing cap?)",
                    gaps,
                    expected,
                )
        elif gap > max(exp * 2.0 + 10.0, start_max_s):
            return BackoffCheckResult(
                False,
                f"gap #{idx} was {gap:.2f} s (too long for continuous doubling from {exp:.0f} s)",
                gaps,
                expected,
            )

    if require_cap and not reached_cap:
        return BackoffCheckResult(
            False,
            f"only {len(gaps)} gap(s) observed; need >= 9 gaps (10 reports) to reach {max_gap_s:.0f} s cap",
            gaps,
            expected,
        )

    summary = ", ".join(f"{g:.1f}s" for g in gaps)
    return BackoffCheckResult(True, f"{len(times)} reports, gaps [{summary}]", gaps, expected)


def check_armed_tamper_pictures(
    events: Sequence[Event],
    min_gap_s: float = TAMPER_PIC_GAP_S,
    jitter_s: float = 2.0,
    min_tamper_triggers: int = 2,
    first_pic_max_delay_s: float = DELIVER_WAIT_S,
) -> ArmedPicCheckResult:
    """Check that armed tamper triggers a picture promptly and at most one per 10 min (600 s)."""
    triggers = [ev.t for ev in events if isinstance(ev, TamperEvent) and ev.active]
    if len(triggers) < min_tamper_triggers:
        return ArmedPicCheckResult(
            False,
            f"need >= {min_tamper_triggers} armed tamper-on (0xB5 P1=1) events, got {len(triggers)}",
            triggers,
            [],
            [],
        )

    all_pics = [ev.t for ev in collect_pictures(events) if ev.t >= triggers[0] - jitter_s]
    if not all_pics:
        return ArmedPicCheckResult(
            False,
            "armed tamper did not produce any picture",
            triggers,
            [],
            [],
        )

    first_delay = all_pics[0] - triggers[0]
    if first_delay > first_pic_max_delay_s:
        return ArmedPicCheckResult(
            False,
            f"first tamper picture arrived {first_delay:.1f} s after first trigger (> {first_pic_max_delay_s:.0f} s)",
            triggers,
            all_pics,
            [],
        )

    pic_gaps = [all_pics[i] - all_pics[i - 1] for i in range(1, len(all_pics))]
    motions = [ev.t for ev in events if isinstance(ev, MotionEvent) and ev.active]
    for idx, gap in enumerate(pic_gaps, start=1):
        if gap < min_gap_s - jitter_s:
            hint = " (PIR motion also fired; cover PIR during test)" if motions else ""
            return ArmedPicCheckResult(
                False,
                f"tamper pictures {idx} and {idx + 1} were {gap:.1f} s apart (< {min_gap_s:.0f} s){hint}",
                triggers,
                all_pics,
                pic_gaps,
            )

    detail = (f"{len(triggers)} tamper trigger(s), {len(all_pics)} picture(s)"
              + (f", gap(s) [{', '.join(f'{g:.1f}s' for g in pic_gaps)}]" if pic_gaps else " (cooldown held)"))
    return ArmedPicCheckResult(True, detail, triggers, all_pics, pic_gaps)


def check_calm_reset(
    disarmed_events: Sequence[Event],
    armed_events: Sequence[Event],
    calm_s: float = TAMPER_CALM_S,
    jitter_s: float = 2.0,
) -> Optional[CalmCheckResult]:
    """Check that >= TAMPER_CALM (600 s) elapsed between the last disarmed 0xB5 and first armed trigger."""
    disarmed_tampers = [ev for ev in disarmed_events if isinstance(ev, TamperEvent)]
    armed_triggers = [ev for ev in armed_events if isinstance(ev, TamperEvent) and ev.active]
    if not disarmed_tampers or not armed_triggers:
        return None
    gap = armed_triggers[0].t - disarmed_tampers[-1].t
    if gap < calm_s - jitter_s:
        return CalmCheckResult(
            False,
            f"armed trigger arrived {gap:.1f} s after last disarmed 0xB5 (< {calm_s:.0f} s calm reset)",
            gap,
        )
    return CalmCheckResult(True, f"calm gap {gap:.1f} s (>= {calm_s:.0f} s)", gap)


def analyze_tamper_log(
    source: Union[str, Iterable[Union[str, Tuple[float, str]]]],
    device_id: Optional[str] = None,
    require_cap: bool = True,
    min_armed_triggers: int = 2,
) -> TamperLogAnalysis:
    """Parse a full gateway log and return TamperLogAnalysis(backoff, calm, armed)."""
    events = parse_gateway_log(source, device_id=device_id)
    disarmed_evs, armed_evs = split_events_by_armed(events)
    tamper_times = [ev.t for ev in disarmed_evs if isinstance(ev, TamperEvent)]
    backoff_res = check_tamper_backoff(tamper_times, require_cap=require_cap)
    calm_res = check_calm_reset(disarmed_evs, armed_evs)
    armed_res = check_armed_tamper_pictures(armed_evs, min_tamper_triggers=min_armed_triggers)
    return TamperLogAnalysis(backoff_res, calm_res, armed_res)


class GatewayLog:
    """Reads the gateway's serial port in the background; masks 32-hex keys in stored lines."""

    def __init__(self, port: str):
        if serial is None:
            raise RuntimeError("pyserial is required for live serial capture: pip install pyserial")
        self.ser = serial.Serial(port, 115200, timeout=0.3)
        self.lines = []
        self.lock = threading.Lock()
        self.stop = False
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        while not self.stop:
            try:
                raw = self.ser.readline()
            except serial.SerialException:
                if self.stop:
                    break
                time.sleep(1)
                try:
                    self.ser.close()
                    self.ser.open()
                except serial.SerialException:
                    pass
                continue
            if raw:
                line = HEX32.sub("<hidden>", raw.decode("utf-8", "replace").rstrip())
                with self.lock:
                    self.lines.append((time.time(), line))

    def mark(self) -> int:
        with self.lock:
            return len(self.lines)

    def snapshot(self, since: int = 0) -> list:
        with self.lock:
            return list(self.lines[since:])

    def wait(self, pattern: str, timeout: float, since: int):
        """First line at index >= since that matches; returns (time, line, index) or None."""
        rx = re.compile(pattern)
        t0 = time.time()
        while time.time() - t0 < timeout:
            with self.lock:
                for i in range(since, len(self.lines)):
                    if rx.search(self.lines[i][1]):
                        return self.lines[i][0], self.lines[i][1], i
            time.sleep(0.2)
        return None

    def send(self, cmd: str):
        self.ser.write((cmd + "\n").encode())

    def close(self):
        self.stop = True
        self._thread.join(timeout=1.5)
        try:
            self.ser.close()
        except Exception:
            pass


def _wait_tamper_calm(log: GatewayLog, dev: str, since_idx: int, margin_s: float = 5.0):
    """Wait until SW3 is closed and TAMPER_CALM_S + margin_s has elapsed since the last 0xB5 report."""
    prompted_close = False
    last_print = 0.0
    deadline = time.time() + TAMPER_MAX_GAP_S + TAMPER_CALM_S + 60.0
    while time.time() < deadline:
        evs = parse_gateway_log(log.snapshot(since_idx), device_id=dev)
        tampers = [e for e in evs if isinstance(e, TamperEvent)]
        if not tampers:
            return
        last = tampers[-1]
        if last.active:
            if not prompted_close:
                print("[*] Last 0xB5 had tamper active=1 (cover open). Close/press SW3 now and "
                      "keep it untouched until the calm reset finishes...", flush=True)
                prompted_close = True
            time.sleep(0.5)
            continue
        prompted_close = False
        rem = (last.t + TAMPER_CALM_S + margin_s) - time.time()
        if rem <= 0:
            print("[*] Tamper calm reset complete (backoff back to 2 s).", flush=True)
            return
        now = time.time()
        if now - last_print >= 30.0:
            print(f"[*] Waiting {rem:.0f} s for TAMPER_CALM ({TAMPER_CALM_S:.0f} s) reset; "
                  f"keep SW3 closed and untouched...", flush=True)
            last_print = now
        time.sleep(min(1.0, rem))


def _run_live(args, dev: str) -> int:
    if serial is None:
        print("[-] pyserial is required for live serial capture: pip install pyserial")
        return 2
    if not args.port:
        from port_selector import GATEWAY_USB, find_usb_port

        args.port = os.environ.get("RECEIVER_PORT") or find_usb_port(GATEWAY_USB)
    if not args.port:
        print("[-] Gateway not found on USB; pass --port")
        return 2
    log = GatewayLog(args.port)
    results = []
    phase1_start = None

    def check(name: str, ok: bool, detail: str) -> bool:
        results.append(ok)
        print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)
        return ok

    try:
        if args.phase in ("all", "backoff"):
            m = log.mark()
            log.send("alarm off")
            r = log.wait(rf"\[Downlink\] -> {dev}: armed 0\b", DELIVER_WAIT_S, m)
            if not check("alarm OFF delivered", r is not None,
                         "camera disarmed" if r else f"no disarmed downlink in {DELIVER_WAIT_S:.0f} s"):
                return 1

            phase1_start = log.mark()
            exp = expected_backoff_sequence(max(1, args.backoff_reports - 1))
            timeout = sum(exp) + 120.0
            print(f"[*] Cover PIR. Toggle tamper switch SW3 repeatedly until {args.backoff_reports} "
                  f"0xB5 reports are logged (timeout {timeout:.0f} s)...", flush=True)
            seen = 0
            t0 = time.time()
            while time.time() - t0 < timeout:
                evs = parse_gateway_log(log.snapshot(phase1_start), device_id=dev)
                tampers = [e for e in evs if isinstance(e, TamperEvent)]
                if len(tampers) > seen:
                    for e in tampers[seen:]:
                        dt = f" (+{e.t - tampers[seen - 1].t:.2f} s)" if seen > 0 else ""
                        seen += 1
                        print(f"    0xB5 #{seen} active={int(e.active)}{dt}", flush=True)
                if seen >= args.backoff_reports:
                    break
                time.sleep(0.25)

            evs = parse_gateway_log(log.snapshot(phase1_start), device_id=dev)
            times = [e.t for e in evs if isinstance(e, TamperEvent)]
            res = check_tamper_backoff(times, require_cap=(args.backoff_reports >= DEFAULT_BACKOFF_REPORTS))
            check("tamper backoff (alarm OFF)", res.ok, res.detail)

        if args.phase in ("all", "armed"):
            m = log.mark()
            log.send("alarm on")
            r = log.wait(rf"\[Downlink\] -> {dev}: armed 1\b", DELIVER_WAIT_S, m)
            if not check("alarm ON delivered", r is not None,
                         "camera armed" if r else f"no armed downlink in {DELIVER_WAIT_S:.0f} s"):
                return 1

            if phase1_start is not None:
                _wait_tamper_calm(log, dev, phase1_start)

            phase_start = log.mark()
            print(f"[*] Keep PIR covered. Trigger tamper (open/close/open) during the next "
                  f"{args.armed_wait:.0f} s...", flush=True)
            t0 = time.time()
            seen_t, seen_p = 0, 0
            while time.time() - t0 < args.armed_wait:
                evs = parse_gateway_log(log.snapshot(phase_start), device_id=dev)
                tampers = [e for e in evs if isinstance(e, TamperEvent) and e.active]
                pics = collect_pictures(evs)
                if len(tampers) > seen_t:
                    for _ in tampers[seen_t:]:
                        print(f"    armed tamper trigger #{seen_t + 1}", flush=True)
                        seen_t += 1
                if len(pics) > seen_p:
                    for p in pics[seen_p:]:
                        print(f"    picture #{seen_p + 1} ({p.kind})", flush=True)
                        seen_p += 1
                time.sleep(0.5)

            evs = parse_gateway_log(log.snapshot(phase_start), device_id=dev)
            res = check_armed_tamper_pictures(evs)
            check("armed tamper picture cooldown (10 min)", res.ok, res.detail)
    finally:
        log.close()

    passed = sum(results)
    print(f"\n{passed}/{len(results)} checks passed")
    return 0 if passed == len(results) else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--port", default=None,
                    help="gateway USB serial port (default: found by USB ID, e.g. COM9 or /dev/ttyACM0)")
    ap.add_argument("--device", default=None,
                    help="camera device ID (default: DEVICE_ID in MD_CAM/firmware/config.h)")
    ap.add_argument("--phase", choices=["all", "backoff", "armed"], default="all",
                    help="which check to run (default: all)")
    ap.add_argument("--backoff-reports", type=int, default=DEFAULT_BACKOFF_REPORTS,
                    help="0xB5 reports to collect for backoff (default 10 reaches 300 s cap)")
    ap.add_argument("--armed-wait", type=float, default=620.0,
                    help="seconds to observe armed tamper picture cooldown (default 620)")
    ap.add_argument("--log", default=None,
                    help="analyze a saved gateway serial log file offline instead of opening serial")
    args = ap.parse_args(argv)

    if args.device:
        import derive_key

        try:
            dev = derive_key.clean_device_id(args.device)
        except ValueError as e:
            print(f"[-] {e}")
            return 2
    elif args.log:
        dev = None
    else:
        import cc1110_tools

        dev = cc1110_tools.config_device_id(CONFIG_H)
        if not dev:
            print("[-] No camera ID: pass --device (no MD_CAM/firmware/config.h)")
            return 2

    if args.log:
        try:
            with open(args.log, encoding="utf-8") as f:
                text = f.read()
        except OSError as e:
            print(f"[-] cannot read {args.log}: {e}")
            return 2
        analysis = analyze_tamper_log(
            text, device_id=dev, require_cap=(args.backoff_reports >= DEFAULT_BACKOFF_REPORTS)
        )
        ok_all = True
        if args.phase in ("all", "backoff"):
            print(f"[{'PASS' if analysis.backoff.ok else 'FAIL'}] tamper backoff (alarm OFF): {analysis.backoff.detail}")
            ok_all = ok_all and analysis.backoff.ok
        if args.phase == "all" and analysis.calm is not None:
            print(f"[{'PASS' if analysis.calm.ok else 'FAIL'}] tamper calm reset: {analysis.calm.detail}")
            ok_all = ok_all and analysis.calm.ok
        if args.phase in ("all", "armed"):
            print(f"[{'PASS' if analysis.armed.ok else 'FAIL'}] armed tamper picture cooldown: {analysis.armed.detail}")
            ok_all = ok_all and analysis.armed.ok
        return 0 if ok_all else 1

    return _run_live(args, dev)


if __name__ == "__main__":
    sys.exit(main())
