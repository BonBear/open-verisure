"""Host tests for MD_CAM/test_tamper.py using synthetic gateway serial logs."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "MD_CAM"))
import test_tamper as tt  # noqa: E402

DEV = "2NCEKS3T"


def _pkt(t, p0, p1, seq, tamper_on=False, device=DEV):
    t_str = "TAMPER (Cover removed!)" if tamper_on else "Secure (Cover closed)"
    return (
        f"[{t:8.3f} s] (+  500 ms) >>> [#0001] [AUTHENTICATED] Device: \"{device}\" | Seq: #{seq} <<<\n"
        f"    Magnet  : CLOSED (Secure)\n"
        f"    Shock   : Clear\n"
        f"    Tamper  : {t_str}\n"
        f"    Battery : 2980 mV (95%)\n"
        f"    Signal  : RSSI -45 dBm | LQI 90 | Freq. error +1.6 kHz | CRC: OK\n"
        f"    Pins    : P0=0x{p0:02X} P1=0x{p1:02X} P2=0x00 (P2_0=0) RST_CAUSE=0\n\n"
    )


def _downlink(t, armed, device=DEV):
    return f"[{t:8.3f} s] (+  100 ms) [Downlink] -> {device}: armed {1 if armed else 0}, cmd 0x00 (poll 1, camera state 0x00)\n"


def _img(kind, device=DEV):
    return f"[ImageReceiver] {kind} from {device}: 4200 bytes, width 320, id 1, 0/105 chunks missing\n"


def test_polls_with_tamper_flag_do_not_count_as_0xb5_reports():
    # A poll (P0=0xBA) prints "Tamper  : TAMPER" while the cover is off, but only P0=0xB5 is a tamper report.
    log = (
        _downlink(1.0, False)
        + _pkt(2.0, 0xB5, 0x01, 100, tamper_on=True)
        + _pkt(4.5, 0xBA, 0x00, 101, tamper_on=True)   # poll while tampered: must be ignored
        + _pkt(4.6, 0xB5, 0x00, 102, tamper_on=False)
    )
    evs = tt.parse_gateway_log(log, device_id=DEV)
    tampers = [e for e in evs if isinstance(e, tt.TamperEvent)]
    assert len(tampers) == 2
    assert [t.t for t in tampers] == [2.0, 4.6]
    assert tampers[0].active is True and tampers[1].active is False


def test_backoff_doubles_from_2s_to_5min_cap_passes():
    # 10 reports -> 9 gaps: 2, 4, 8, 16, 32, 64, 128, 256, 300 s (+0.5 s debounce/jitter)
    t = 10.0
    lines = [_downlink(1.0, False), _pkt(t, 0xB5, 0x01, 1, tamper_on=True)]
    for i, gap in enumerate([2.5, 4.5, 8.5, 16.5, 32.5, 64.5, 128.5, 256.5, 300.5], start=2):
        t += gap
        lines.append(_pkt(t, 0xB5, i % 2, i, tamper_on=bool(i % 2)))
    # Extra report at cap stays at 300 s
    t += 300.5
    lines.append(_pkt(t, 0xB5, 1, 11, tamper_on=True))

    res = tt.analyze_tamper_log("".join(lines), device_id=DEV).backoff
    assert res.ok, res.detail
    assert len(res.gaps) == 10
    assert res.expected_gaps[-1] == 300.0


def test_backoff_rejects_no_doubling():
    t = 10.0
    lines = [_downlink(1.0, False), _pkt(t, 0xB5, 0x01, 1, tamper_on=True)]
    for i in range(2, 11):
        t += 2.2
        lines.append(_pkt(t, 0xB5, i % 2, i, tamper_on=bool(i % 2)))
    res = tt.analyze_tamper_log("".join(lines), device_id=DEV).backoff
    assert not res.ok
    assert "gap #2" in res.detail


def test_backoff_rejects_missing_5min_cap():
    t = 10.0
    lines = [_downlink(1.0, False), _pkt(t, 0xB5, 0x01, 1, tamper_on=True)]
    for i, gap in enumerate([2.2, 4.2, 8.2, 16.2, 32.2, 64.2, 128.2, 256.2, 512.2], start=2):
        t += gap
        lines.append(_pkt(t, 0xB5, i % 2, i, tamper_on=bool(i % 2)))
    res = tt.analyze_tamper_log("".join(lines), device_id=DEV).backoff
    assert not res.ok
    assert "cap" in res.detail


def test_armed_tamper_one_picture_per_10min_passes():
    # First tamper triggers a picture (0xB2 + ImageReceiver); cover closes (P1=0), re-opens at +120 s
    # (no 2nd picture), closes, then re-opens at +615 s -> triggers a second picture -> allowed.
    log = (
        _downlink(5.0, True)
        + _pkt(10.0, 0xB5, 0x01, 10, tamper_on=True)
        + _pkt(12.0, 0xB2, 0xD5, 11, tamper_on=True)
        + _img("JPEG")
        + _pkt(80.0, 0xB5, 0x00, 12, tamper_on=False)
        + _pkt(130.0, 0xB5, 0x01, 13, tamper_on=True)    # inside 10 min: no picture
        + _pkt(300.0, 0xB5, 0x00, 14, tamper_on=False)
        + _pkt(615.0, 0xB5, 0x01, 15, tamper_on=True)    # after 10 min: picture allowed
        + _pkt(617.0, 0xB2, 0xD5, 16, tamper_on=True)
        + _img("JPEG")
    )
    evs = tt.parse_gateway_log(log, device_id=DEV)
    _, armed_evs = tt.split_events_by_armed(evs)
    res = tt.check_armed_tamper_pictures(armed_evs)
    assert res.ok, res.detail
    assert len(res.picture_times) == 2


def test_armed_tamper_resend_does_not_count_as_second_picture():
    log = (
        _downlink(5.0, True)
        + _pkt(10.0, 0xB5, 0x01, 10, tamper_on=True)
        + _pkt(12.0, 0xB2, 0xD5, 11, tamper_on=True)
        + _img("JPEG")
        + "[ImageReceiver] Picture from 2NCEKS3T complete after resend: 4200 bytes, 1 round(s), 400 ms\n"
        + _img("JPEG")
        + _pkt(80.0, 0xB5, 0x00, 12, tamper_on=False)
        + _pkt(120.0, 0xB5, 0x01, 13, tamper_on=True)
    )
    evs = tt.parse_gateway_log(log, device_id=DEV)
    _, armed_evs = tt.split_events_by_armed(evs)
    res = tt.check_armed_tamper_pictures(armed_evs)
    assert res.ok, res.detail
    assert len(res.picture_times) == 1


def test_armed_tamper_rejects_second_picture_within_10min():
    log = (
        _downlink(5.0, True)
        + _pkt(10.0, 0xB5, 0x01, 10, tamper_on=True)
        + _pkt(12.0, 0xB2, 0xD5, 11, tamper_on=True)
        + _img("JPEG")
        + _pkt(80.0, 0xB5, 0x00, 12, tamper_on=False)
        + _pkt(120.0, 0xB5, 0x01, 13, tamper_on=True)
        + _pkt(122.0, 0xB2, 0xD5, 14, tamper_on=True)   # bug: second tamper picture at 110 s
        + _img("JPEG")
    )
    evs = tt.parse_gateway_log(log, device_id=DEV)
    _, armed_evs = tt.split_events_by_armed(evs)
    res = tt.check_armed_tamper_pictures(armed_evs)
    assert not res.ok
    assert "110.0 s apart" in res.detail


def test_armed_tamper_rejects_missing_picture():
    log = (
        _downlink(5.0, True)
        + _pkt(10.0, 0xB5, 0x01, 10, tamper_on=True)
        + _pkt(80.0, 0xB5, 0x00, 11, tamper_on=False)
        + _pkt(120.0, 0xB5, 0x01, 12, tamper_on=True)
    )
    evs = tt.parse_gateway_log(log, device_id=DEV)
    _, armed_evs = tt.split_events_by_armed(evs)
    res = tt.check_armed_tamper_pictures(armed_evs)
    assert not res.ok
    assert "did not produce any picture" in res.detail


def test_armed_tamper_rejects_missing_first_picture():
    # triggers=[10, 620] with only a single picture at 622 (first trigger failed to take a picture)
    log = (
        _downlink(5.0, True)
        + _pkt(10.0, 0xB5, 0x01, 10, tamper_on=True)
        + _pkt(300.0, 0xB5, 0x00, 11, tamper_on=False)
        + _pkt(620.0, 0xB5, 0x01, 12, tamper_on=True)
        + _pkt(622.0, 0xB2, 0xD5, 13, tamper_on=True)
        + _img("JPEG")
    )
    evs = tt.parse_gateway_log(log, device_id=DEV)
    _, armed_evs = tt.split_events_by_armed(evs)
    res = tt.check_armed_tamper_pictures(armed_evs)
    assert not res.ok
    assert "first tamper picture arrived" in res.detail


def _both_phases_log(calm_wait_s: float) -> str:
    t = 10.0
    parts = [_downlink(1.0, False), _pkt(t, 0xB5, 0x01, 1, tamper_on=True)]
    for i, gap in enumerate([2.3, 4.3, 8.3, 16.3, 32.3, 64.3, 128.3, 256.3, 300.3], start=2):
        t += gap
        parts.append(_pkt(t, 0xB5, i % 2, i, tamper_on=bool(i % 2)))   # ends at i=10 with P1=0
    parts.append(_downlink(t + 20.0, True))
    t_arm = t + calm_wait_s
    parts.append(_pkt(t_arm, 0xB5, 0x01, 20, tamper_on=True))
    parts.append(_pkt(t_arm + 2.0, 0xB2, 0xD5, 21, tamper_on=True))
    parts.append(_img("JPEG"))
    parts.append(_pkt(t_arm + 40.0, 0xB5, 0x00, 22, tamper_on=False))
    parts.append(_pkt(t_arm + 80.0, 0xB5, 0x01, 23, tamper_on=True))
    return "".join(parts)


def test_offline_log_cli_runs_both_phases(tmp_path, capsys):
    p = tmp_path / "tamper.log"
    p.write_text(_both_phases_log(calm_wait_s=605.0), encoding="utf-8")
    assert tt.main(["--log", str(p), "--device", DEV]) == 0
    out = capsys.readouterr().out
    assert "[PASS] tamper backoff" in out
    assert "[PASS] tamper calm reset" in out
    assert "[PASS] armed tamper picture cooldown" in out


def test_calm_wait_rejects_early_armed_trigger(tmp_path, capsys):
    p = tmp_path / "tamper_early.log"
    p.write_text(_both_phases_log(calm_wait_s=25.0), encoding="utf-8")
    analysis = tt.analyze_tamper_log(p.read_text(encoding="utf-8"), device_id=DEV)
    assert analysis.calm is not None and not analysis.calm.ok
    assert tt.main(["--log", str(p), "--device", DEV]) == 1
    assert "[FAIL] tamper calm reset" in capsys.readouterr().out


def test_live_mode_friendly_error_and_log_mode_without_pyserial(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(tt, "serial", None)
    monkeypatch.setitem(sys.modules, "port_selector", None)
    assert tt.main(["--device", DEV]) == 2
    assert "pyserial is required" in capsys.readouterr().out

    p = tmp_path / "ok.log"
    p.write_text(_both_phases_log(calm_wait_s=605.0), encoding="utf-8")
    assert tt.main(["--log", str(p)]) == 0
