"""tools/saleae_uart_decode.py: 8O1/8N1 decoding from a Logic 2 raw digital CSV export."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import saleae_uart_decode as dec

BAUD = 115200.0
BIT = 1.0 / BAUD


def frame_levels(value, parity="odd", bits=8):
    """Line levels for one frame: start, data LSB first, parity, stop."""
    data = [(value >> k) & 1 for k in range(bits)]
    out = [0] + data
    if parity != "none":
        ones = sum(data)
        out.append(1 if (ones % 2 == 0) == (parity == "odd") else 0)
    return out + [1]


def write_capture(tmp_path, frames, parity="odd", channel=1, channels=2, start=0.001, gap=200e-6):
    """frames: list of byte values sent back to back with `gap` idle between them."""
    t = start
    bits = []
    for v in frames:
        for lvl in frame_levels(v, parity):
            bits.append((t, lvl))
            t += BIT
        t += gap
    rows = ["Time [s]," + ",".join(f"Channel {i}" for i in range(channels))]

    def row(time, lvl):
        levels = ["1"] * channels
        levels[channel] = str(lvl)
        return f"{time:.9f}," + ",".join(levels)

    rows.append(row(0.0, 1))
    prev = 1
    for time, lvl in bits:
        if lvl != prev:
            rows.append(row(time, lvl))
            prev = lvl
    rows.append(row(t + 0.001, 1))
    p = tmp_path / "digital.csv"
    p.write_text("\n".join(rows) + "\n", encoding="utf-8")
    return p


def test_decodes_ic301_command_8o1(tmp_path, capsys):
    p = write_capture(tmp_path, [0xFF, 0xFE, 0x81, 0x0B, 0x0C])
    assert dec.main([str(p), "--channel", "1"]) == 0
    out = capsys.readouterr().out
    assert [line.split()[-1] for line in out.splitlines()[:5] if "0x" in line] == [
        "0xFF", "0xFE", "0x81", "0x0B", "0x0C"]
    assert "5 bytes, 0 with framing/parity errors" in out


def test_wrong_parity_is_flagged(tmp_path, capsys):
    p = write_capture(tmp_path, [0x81, 0x0C], parity="even")
    assert dec.main([str(p), "--channel", "1"]) == 1
    assert "PARITY" in capsys.readouterr().out


def test_8n1(tmp_path):
    p = write_capture(tmp_path, [0x55, 0xA5, 0x00], parity="none")
    _, rows = dec.load_transitions(str(p))
    frames = dec.decode(dec.channel_edges(rows, 1), BAUD, 8, "none")
    assert [f["value"] for f in frames] == [0x55, 0xA5, 0x00]
    assert all(f["stop_ok"] for f in frames)


def test_edges_listing(tmp_path, capsys):
    p = write_capture(tmp_path, [0xF8])
    assert dec.main([str(p), "--edges"]) == 0
    out = capsys.readouterr().out
    assert "Channel 0: start 1, 0 edges" in out
    assert "Channel 1: start 1," in out


def test_bad_channel(tmp_path):
    p = write_capture(tmp_path, [0x01])
    assert dec.main([str(p), "--channel", "5"]) == 2
