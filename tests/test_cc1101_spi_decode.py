"""tools/cc1101_spi_decode.py: Saleae CSV and hex-line input, names, register image, derived values."""
import json
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import cc1101_spi_decode as dec  # noqa: E402


def saleae(txns, header='name,type,start_time,duration,"mosi","miso"'):
    """txns: list of byte lists (MOSI); MISO echoes 0x0F for every byte."""
    rows = [header]
    t = 0.0
    for bs in txns:
        rows.append(f'"SPI",enable,{t:.6f},0.0001,,')
        for b in bs:
            t += 0.00001
            rows.append(f'"SPI",result,{t:.6f},0.00001,0x{b:02X},0x0F')
        rows.append(f'"SPI",disable,{t + 0.00001:.6f},0.0001,,')
        t += 0.001
    return "\n".join(rows) + "\n"


def run(tmp_path, text, *args):
    p = tmp_path / "cap.csv"
    p.write_text(text, encoding="utf-8")
    return dec.main([str(p), *args])


def door_capture():
    return [
        [0x30],                                   # SRES
        [0x40 | 0x0D, 0x20, 0x2E, 0xEF],          # FREQ2..0 burst
        [0x40 | 0x10, 0x8A, 0x75],                # MDMCFG4, MDMCFG3 burst
        [0x15, 0x35],                             # DEVIATN single
        [0x04, 0xD3], [0x05, 0x91],               # SYNC1, SYNC0
        [0x7E, 0xC0, 0x60],                       # PATABLE burst
        [0x36],                                   # SIDLE
        [0x34],                                   # SRX
        [0xF5, 0x00],                             # read MARCSTATE
    ]


def test_saleae_door_sensor(tmp_path, capsys):
    assert run(tmp_path, saleae(door_capture()), "--xtal-mhz", "27", "--json") == 0
    out = json.loads(capsys.readouterr().out)
    r = out["registers"]
    assert (r["FREQ2"], r["FREQ1"], r["FREQ0"]) == ("0x20", "0x2E", "0xEF")
    assert r["MDMCFG4"] == "0x8A" and r["MDMCFG3"] == "0x75" and r["DEVIATN"] == "0x35"
    assert r["SYNC1"] == "0xD3" and r["SYNC0"] == "0x91"
    assert out["patable"][:2] == ["0xC0", "0x60"] and out["patable"][2] is None
    d = out["derived"]
    assert abs(d["carrier_mhz"] - 868.950) < 0.001
    assert abs(d["data_rate_baud"] - 38418) < 5     # (256+117)*2^10/2^28*27 MHz
    assert abs(d["deviation_khz"] - 21.4) < 0.1
    assert d["sync_word"] == "0xD391"
    assert abs(d["rx_filter_bw_khz"] - 210.9) < 0.2  # MDMCFG4 0x8A: E=2, M=0
    assert out["warnings"] == []


def test_saleae_log_names(tmp_path, capsys):
    assert run(tmp_path, saleae(door_capture()), "--xtal-mhz", "27") == 0
    o = capsys.readouterr().out
    for s in ("STROBE SRES", "STROBE SIDLE", "STROBE SRX", "READ  status MARCSTATE",
              "WRITE burst FREQ2", "FREQ1=0x2E", "WRITE PATABLE burst: C0 60", "SYNC0=0x91"):
        assert s in o
    assert "868.9500 MHz" in o and "0xD391" in o
    assert "0x0D FREQ2" in o and "0x2E TEST0" in o


def test_default_xtal_is_26(tmp_path, capsys):
    run(tmp_path, saleae([[0x4D, 0x20, 0x2E, 0xEF]]), "--json")
    d = json.loads(capsys.readouterr().out)["derived"]
    assert d["xtal_mhz"] == 26.0
    assert abs(d["carrier_mhz"] - 26 / 27 * 868.950) < 0.001


def test_header_variants(tmp_path, capsys):
    text = saleae([[0x30], [0x4D, 0x20, 0x2E, 0xEF]], header="Name,TYPE,Start_Time,Duration,MOSI,MISO")
    assert run(tmp_path, text, "--xtal-mhz", "27", "--json") == 0
    assert json.loads(capsys.readouterr().out)["registers"]["FREQ2"] == "0x20"
    quoted = saleae([[0x30]], header='"name","type","start_time","duration","mosi","miso"')
    assert run(tmp_path, quoted) == 0
    assert "STROBE SRES" in capsys.readouterr().out


def test_hex_line_format(tmp_path, capsys):
    text = "# pasted\n30\n47 2E 80\n0x7E 0xC0 0x60\n\nB5 00\n"
    assert run(tmp_path, text, "--xtal-mhz", "27", "--json") == 0
    out = json.loads(capsys.readouterr().out)
    assert out["transactions"] == 4
    assert out["registers"]["PKTCTRL1"] == "0x2E" and out["registers"]["PKTCTRL0"] == "0x80"
    assert out["patable"][0] == "0xC0"


def test_empty_file(tmp_path, capsys):
    assert run(tmp_path, "") == 0
    o = capsys.readouterr().out
    assert "0 transactions" in o and "FREQ2" in o
    assert run(tmp_path, "", "--json") == 0
    assert json.loads(capsys.readouterr().out)["registers"] == {}


def test_missing_file(tmp_path):
    assert dec.main([str(tmp_path / "nope.csv")]) == 2


def test_malformed_lines_skipped(tmp_path, capsys):
    assert run(tmp_path, "36\nZZ 12\n4D 20 2E EF\nnot hex\n") == 0
    cap = capsys.readouterr()
    assert "FREQ0=0xEF" in cap.out and "STROBE SIDLE" in cap.out
    assert cap.err.count("bad hex byte") == 2


def test_malformed_saleae_rows(tmp_path, capsys):
    text = saleae([[0x30]]) + '"SPI",result,1.0,0.1,0xZZ,0x00\n"SPI",result\n'
    assert run(tmp_path, text) == 0
    cap = capsys.readouterr()
    assert "STROBE SRES" in cap.out
    assert "bad byte value" in cap.err and "too few columns" in cap.err


def test_burst_past_last_register_does_not_wrap(tmp_path, capsys):
    # burst at 0x2D with 4 bytes: TEST1, TEST0 are written, the rest must not hit strobes
    assert run(tmp_path, saleae([[0x40 | 0x2D, 0x11, 0x22, 0x33, 0x44]]), "--json") == 0
    out = json.loads(capsys.readouterr().out)
    assert out["registers"] == {"TEST1": "0x11", "TEST0": "0x22"}
    assert any("past 0x2E" in w and "2 byte(s) ignored" in w for w in out["warnings"])


def test_truncated_transaction(tmp_path, capsys):
    assert run(tmp_path, "4D\n", "--json") == 0   # burst write header only
    out = json.loads(capsys.readouterr().out)
    assert out["registers"] == {}
    assert any("truncated" in w for w in out["warnings"])


def test_strobe_names_and_status(tmp_path, capsys):
    run(tmp_path, "\n".join(f"{a:02X}" for a in range(0x30, 0x3E)) + "\nF1 00\nFB 00\n")
    o = capsys.readouterr().out
    names = [dec.STROBES[a] for a in range(0x30, 0x3E)]
    assert names == ["SRES", "SFSTXON", "SXOFF", "SCAL", "SRX", "STX", "SIDLE", "SAFC",
                     "SWOR", "SPWD", "SFRX", "SFTX", "SWORRST", "SNOP"]
    for n in names:
        assert f"STROBE {n} " in o
    assert "status VERSION" in o and "status RXBYTES" in o
    assert len(dec.REGS) == 0x2F and dec.REGS[0x2E] == "TEST0" and dec.REGS[0x15] == "DEVIATN"


def test_fifo_and_status_write(tmp_path, capsys):
    run(tmp_path, "7F 05 AA BB\nFF 00 00\nF0\n70 01\n")
    cap = capsys.readouterr()
    assert "WRITE TX FIFO burst: 3 byte(s) = 05 AA BB" in cap.out
    assert "READ  RX FIFO burst" in cap.out
    assert "INVALID write to status register PARTNUM" in cap.out


def test_wor_timeout(tmp_path, capsys):
    # EVENT0 = 0x876B, WOR_RES = 0 at 27 MHz: 750/27e6*34667 = 0.963 s
    run(tmp_path, "5E 87 6B\n60 00\n16 03\n", "--xtal-mhz", "27", "--json")  # WOREVT1/0, WORCTRL, MCSM2
    w = json.loads(capsys.readouterr().out)["derived"]["wor"]
    assert w["event0"] == 0x876B
    assert abs(w["event0_timeout_ms"] - 750 / 27e6 * 0x876B * 1e3) < 0.01
    assert w["event1_clocks"] == 4 and w["rx_time_code"] == 3


@pytest.mark.parametrize("reg,val,key,expect", [
    (0x12, 0x30, "modulation", "ASK/OOK"),
    (0x12, 0x03, "sync_mode", "30/32 sync word bits"),
])
def test_modulation_fields(tmp_path, capsys, reg, val, key, expect):
    run(tmp_path, f"{reg:02X} {val:02X}\n", "--json")
    assert json.loads(capsys.readouterr().out)["derived"][key] == expect


def test_packet_fields(tmp_path, capsys):
    run(tmp_path, "48 45 05\n06 3D\n", "--json")  # PKTCTRL0=0x45 (whiten, CRC, variable), PKTLEN=0x3D
    d = json.loads(capsys.readouterr().out)["derived"]
    assert d["whitening"] and d["crc_enabled"] and d["pktlen"] == 0x3D
    assert d["packet_length_mode"].startswith("variable")


@pytest.mark.parametrize("hdr", [0x2F, 0x6F, 0xAF, 0xEF])
def test_address_2f_is_not_a_register(tmp_path, capsys, hdr):
    assert run(tmp_path, f"{hdr:02X} 00\n36\n", "--json") == 0
    out = json.loads(capsys.readouterr().out)
    assert out["registers"] == {}
    assert any("no register at 0x2F" in w for w in out["warnings"])


def test_missing_miso_column_prints_no_value(tmp_path, capsys):
    text = ("name,type,start_time,duration,mosi\n"
            '"SPI",enable,0.0,0.1,\n"SPI",result,0.1,0.1,0x8D\n"SPI",result,0.2,0.1,0x00\n'
            '"SPI",disable,0.3,0.1,\n')
    assert run(tmp_path, text) == 0
    o = capsys.readouterr().out
    assert "READ  FREQ2 (0x0D)" in o and "= 00" not in o


def test_blank_miso_value_prints_no_value(tmp_path, capsys):
    text = saleae([[0xF5, 0x00]]).replace(",0x0F\n", ",\n")
    assert run(tmp_path, text) == 0
    o = capsys.readouterr().out
    assert "READ  status MARCSTATE (0x35)" in o and "= 00" not in o


def test_saleae_requires_0x_prefix(tmp_path, capsys):
    text = saleae([[0x30]]) + '"SPI",result,1.0,0.1,10,0x00\n'
    assert run(tmp_path, text) == 0
    assert "0x prefix required" in capsys.readouterr().err


def test_saleae_without_enable_disable_rows(tmp_path, capsys):
    text = 'name,type,start_time,duration,"mosi","miso"\n"SPI",result,0.0,0.1,0x30,0x0F\n'
    assert run(tmp_path, text) == 0
    assert "no enable/disable rows" in capsys.readouterr().err


def test_fsctrl0_offset(tmp_path, capsys):
    # FREQ 0x202EEF, FSCTRL0 = 0xFF = -1 step = -27e6/2^14 Hz
    run(tmp_path, "4D 20 2E EF\n0C FF\n", "--xtal-mhz", "27", "--json")
    d = json.loads(capsys.readouterr().out)["derived"]
    assert abs(d["freq_offset_hz"] + 27e6 / 2**14) < 0.01
    assert abs(d["carrier_with_offset_mhz"] - d["carrier_mhz"] + 27e6 / 2**14 / 1e6) < 1e-9
    run(tmp_path, "4D 20 2E EF\n0C 02\n", "--xtal-mhz", "27")
    o = capsys.readouterr().out
    assert "carrier            868.9500 MHz" in o and "carrier + FSCTRL0" in o


def test_utf16_and_binary_input(tmp_path, capsys):
    p = tmp_path / "u16.txt"
    p.write_text("30\n", encoding="utf-16")
    assert dec.main([str(p)]) == 0
    assert "STROBE SRES" in capsys.readouterr().out
    b = tmp_path / "bin.csv"
    b.write_bytes(b"\xff\x00\x80\x81")
    assert dec.main([str(b)]) == 2


@pytest.mark.parametrize("x", ["0", "-27"])
def test_bad_xtal_rejected(tmp_path, x):
    p = tmp_path / "c.txt"
    p.write_text("30\n")
    with pytest.raises(SystemExit):
        dec.main([str(p), "--xtal-mhz", x])
