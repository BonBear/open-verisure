"""tools/si446x_spi_decode.py: Saleae CSV and hex-line input, command log, property image, derived values."""
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import si446x_spi_decode as dec


def saleae(txns, header='name,type,start_time,duration,"mosi","miso"'):
    """txns: list of (MOSI bytes, MISO bytes or None for all zero)."""
    rows = [header]
    t = 0.0
    for mosi, miso in txns:
        miso = miso if miso is not None else [0] * len(mosi)
        rows.append(f'"SPI",enable,{t:.6f},0.0001,,')
        for m, s in zip(mosi, miso):
            t += 0.00001
            rows.append(f'"SPI",result,{t:.6f},0.00001,0x{m:02X},0x{s:02X}')
        rows.append(f'"SPI",disable,{t + 0.00001:.6f},0.0001,,')
        t += 0.001
    return "\n".join(rows) + "\n"


def run(tmp_path, text, *args):
    p = tmp_path / "cap.csv"
    p.write_text(text, encoding="utf-8")
    return dec.main([str(p), *args])


def jrun(tmp_path, capsys, text, *args):
    assert run(tmp_path, text, "--json", *args) == 0
    return json.loads(capsys.readouterr().out)


POWER_UP_26 = "02 01 00 01 8C BA 80\n"   # XO_FREQ 0x018CBA80 = 26 MHz


def test_hex_line_format(tmp_path, capsys):
    text = "# pasted\n02 01 00 01 C9 C3 80\n\n0x11 0x20 0x01 0x51 0x08\n34 03\n"
    out = jrun(tmp_path, capsys, text)
    assert out["transactions"] == 3
    assert out["properties"]["0x2051"] == {"name": "MODEM_CLKGEN_BAND", "value": "0x08"}
    assert out["derived"]["xtal_mhz"] == 30.0


def test_saleae_csv_log(tmp_path, capsys):
    text = saleae([([0x34, 0x03], None), ([0x11, 0x22, 0x01, 0x01, 0x7F], None)])
    assert run(tmp_path, text) == 0
    o = capsys.readouterr().out
    assert "CHANGE_STATE -> READY" in o
    assert "SET PA_PWR_LVL (0x2201) = 7F" in o
    assert "== Final property image" in o and "== TX/RX payloads" in o and "== Derived settings" in o
    assert "PA_PWR_LVL" in o.split("== Derived settings")[1] and "0x7F" in o


def test_header_variants(tmp_path, capsys):
    text = saleae([([0x34, 0x08], None)], header="Name,TYPE,Start_Time,Duration,MOSI,MISO")
    assert run(tmp_path, text) == 0
    assert "CHANGE_STATE -> RX" in capsys.readouterr().out


def test_power_up_sets_xtal(tmp_path, capsys):
    out = jrun(tmp_path, capsys, POWER_UP_26 + "11 20 01 51 08\n")
    assert out["derived"]["xtal_mhz"] == 26.0
    assert not any("assuming" in w for w in out["warnings"])
    run(tmp_path, POWER_UP_26)
    assert "POWER_UP BOOT_OPTIONS=0x01 (PATCH=0, FUNC=1), XTAL_OPTIONS=0x00 (TCXO=0), " \
           "XO_FREQ=26000000 Hz" in capsys.readouterr().out
    # explicit --xtal-mhz wins over POWER_UP
    assert jrun(tmp_path, capsys, POWER_UP_26, "--xtal-mhz", "27")["derived"]["xtal_mhz"] == 27.0


def test_default_xtal_warns(tmp_path, capsys):
    out = jrun(tmp_path, capsys, "11 22 01 01 7F\n")
    assert out["derived"]["xtal_mhz"] == 30.0
    assert any("assuming 30.0 MHz" in w for w in out["warnings"])


def test_set_property_multibyte_and_mismatch(tmp_path, capsys):
    assert run(tmp_path, "11 20 03 03 00 4E 20\n11 20 02 03 AA BB CC\n11 20 01 03 11\n") == 0
    cap = capsys.readouterr()
    assert "SET MODEM_DATA_RATE (0x2003..0x2005) = 00 4E 20" in cap.out
    assert "NUM_PROPS=2 but 3 data byte(s)" in cap.err
    out = jrun(tmp_path, capsys, "11 20 03 03 00 4E 20\n11 20 02 03 AA BB CC\n11 20 01 03 11\n")
    p = out["properties"]
    # last write wins per byte; bytes beyond NUM_PROPS are dropped (0xCC never reaches 0x2005)
    assert (p["0x2003"]["value"], p["0x2004"]["value"], p["0x2005"]["value"]) == ("0x11", "0xBB", "0x20")
    assert p["0x2003"]["name"] == "MODEM_DATA_RATE"


def test_unknown_property_names(tmp_path, capsys):
    assert run(tmp_path, "11 20 01 40 01\n11 77 01 05 02\n") == 0
    o = capsys.readouterr().out
    assert "SET MODEM_0x40 (0x2040) = 01" in o and "SET GROUP_0x77_0x05 (0x7705) = 02" in o


def test_get_property_reply_pairing(tmp_path, capsys):
    text = saleae([
        ([0x12, 0x20, 0x03, 0x03], None),
        ([0x44, 0, 0, 0, 0], [0, 0x00, 0, 0, 0]),             # CTS 0x00: ignored
        ([0x44, 0, 0, 0, 0], [0, 0xFF, 0x00, 0x4E, 0x20]),    # ready: DATA_RATE reply
    ])
    assert run(tmp_path, text) == 0
    o = capsys.readouterr().out
    assert "GET MODEM_DATA_RATE (0x2003..0x2005)" in o
    assert "GET reply MODEM_DATA_RATE (0x2003..0x2005) = 00 4E 20" in o
    assert "CTS=0x00" not in o and "1 READ_CMD_BUFF poll(s)" in o
    out = jrun(tmp_path, capsys, text, "--verbose")
    assert out["cts_not_ready_polls"] == 1 and out["properties"] == {}   # reads are not imaged
    assert run(tmp_path, text, "--verbose") == 0
    assert "CTS=0x00 (not ready)" in capsys.readouterr().out


def test_part_info_reply(tmp_path, capsys):
    text = saleae([([0x01], None),
                   ([0x44] + [0] * 9, [0, 0xFF, 0x22, 0x44, 0x60, 0x0F, 0x00, 0x00, 0x00, 0x06])])
    out = jrun(tmp_path, capsys, text)
    assert out["derived"]["part"] == "0x4460"
    assert run(tmp_path, text) == 0
    o = capsys.readouterr().out
    assert "CHIPREV=0x22, PART=0x4460, PBUILD=0x0F" in o and "ROMID=0x06" in o and "Si4460" in o


def test_int_status_and_device_state_replies(tmp_path, capsys):
    text = saleae([([0x20, 0, 0, 0], None),
                   ([0x44] + [0] * 9, [0, 0xFF, 0, 0, 0x10, 0x10, 0, 0, 0, 0]),
                   ([0x33], None), ([0x44, 0, 0, 0], [0, 0xFF, 0x08, 0x05])])
    assert run(tmp_path, text) == 0
    o = capsys.readouterr().out
    assert "PH_PEND=0x10, PH_STATUS=0x10" in o and "state RX, CURRENT_CHANNEL=0x05" in o


def test_derived_carrier_26mhz(tmp_path, capsys):
    frac = round((868.95e6 / (2 * 26e6 / 4) - 65) * 2**19)
    text = (POWER_UP_26 + "11 20 01 51 08\n"
            f"11 40 04 00 41 {frac >> 16:02X} {(frac >> 8) & 0xFF:02X} {frac & 0xFF:02X}\n")
    d = jrun(tmp_path, capsys, text)["derived"]
    assert d["outdiv"] == 4 and d["nprescaler"] == 2 and d["inte"] == 0x41
    assert abs(d["carrier_hz"] - 868.95e6) < 50
    assert run(tmp_path, text) == 0
    assert "carrier            868.9" in capsys.readouterr().out


def test_frac_out_of_range_warns(tmp_path, capsys):
    out = jrun(tmp_path, capsys, "11 20 01 51 08\n11 40 04 00 41 00 00 10\n", "--xtal-mhz", "26")
    assert any("outside [1, 2)" in w for w in out["warnings"])


def test_derived_data_rate_and_deviation(tmp_path, capsys):
    text = ("11 20 03 03 0F 42 40\n"             # DATA_RATE = 1,000,000
            "11 20 04 06 01 C9 C3 80\n"          # TX_NCO_MODE: NCOMOD 30,000,000, TXOSR code 0
            "11 20 01 51 08\n"
            "11 20 03 0A 00 07 E0\n")            # FREQ_DEV = 2016
    d = jrun(tmp_path, capsys, text, "--xtal-mhz", "30")["derived"]
    assert abs(d["data_rate_bps"] - 100000) < 0.01
    assert abs(d["deviation_hz"] - 2016 * 2 * 30e6 / 4 / 2**19) < 0.01
    d = jrun(tmp_path, capsys, "11 20 04 06 0C C9 C3 80\n11 20 03 03 0F 42 40\n", "--xtal-mhz", "30")
    assert "data_rate_bps" not in d["derived"] and any("reserved" in w for w in d["warnings"])


def test_derived_modulation_sync_crc(tmp_path, capsys):
    text = ("11 20 01 00 0B\n"                   # MOD_TYPE 3 (2GFSK), source 1 (DIRECT)
            "11 11 03 00 01 AA 55\n"              # SYNC_CONFIG len 2 bytes, bytes AA 55
            "11 10 01 00 08\n"
            "11 12 01 00 85\n"                    # CRC_CONFIG: CCITT-16 (5), seed all ones
            "11 12 01 06 03\n")
    d = jrun(tmp_path, capsys, text)["derived"]
    assert d["modulation"] == "2GFSK" and d["mod_source"] == "DIRECT"
    assert d["sync_length_bytes"] == 2 and d["sync_word"] == "AA 55" and d["preamble_tx_length"] == 8
    assert d["crc"] == "CCITT-16" and d["crc_seed_all_ones"]
    assert d["bit_order"] == "LSB first" and d["crc_endian"] == "MSB first"


def test_tx_rx_fifo_payloads(tmp_path, capsys):
    text = saleae([([0x66, 0x05, 0xAA, 0xBB], None),
                   ([0x77, 0, 0], [0, 0x12, 0x34])])
    out = jrun(tmp_path, capsys, text)
    assert out["tx_packets"] == ["05 AA BB"] and out["rx_packets"] == ["12 34"]
    assert run(tmp_path, text) == 0
    o = capsys.readouterr().out
    assert "WRITE_TX_FIFO: 3 byte(s) = 05 AA BB" in o and "TX #1 (3 bytes): 05 AA BB" in o
    assert "READ_RX_FIFO: 2 byte(s) = 12 34" in o and "RX #1 (2 bytes): 12 34" in o


def test_commands_state_and_gpio(tmp_path, capsys):
    assert run(tmp_path, "31 00 70 00 10\n32 00 00 00 20 08 03 03\n13 41 00 00 00 00 00 00\n"
                         "50 00\n") == 0
    o = capsys.readouterr().out
    assert "START_TX CHANNEL=0, CONDITION=0x70 (TXCOMPLETE_STATE=TX), TX_LEN=16" in o
    assert "RX_LEN=32" in o and "RXTIMEOUT_STATE=RX" in o
    assert "GPIO0=0x41 (mode 1, pull 1)" in o
    assert "READ FRR_A_READ (0x50)" in o


def test_unknown_command(tmp_path, capsys):
    assert run(tmp_path, "AB 01 02\nAC\n") == 0
    o = capsys.readouterr().out
    assert "UNKNOWN 0xAB args 01 02" in o and "UNKNOWN 0xAC\n" in o


def test_empty_file(tmp_path, capsys):
    assert run(tmp_path, "") == 0
    assert "0 transactions" in capsys.readouterr().out
    out = jrun(tmp_path, capsys, "")
    assert out["properties"] == {} and out["tx_packets"] == [] and out["warnings"] == []


def test_malformed_and_missing_input(tmp_path, capsys):
    assert dec.main([str(tmp_path / "nope.csv")]) == 2
    assert run(tmp_path, "34 03\nZZ 12\n") == 0
    assert capsys.readouterr().err.count("bad hex byte") == 1
    p = tmp_path / "u16.txt"
    p.write_text("34 03\n", encoding="utf-16")
    assert dec.main([str(p)]) == 0
    assert "CHANGE_STATE -> READY" in capsys.readouterr().out
    b = tmp_path / "bin.csv"
    b.write_bytes(b"\xff\x00\x80\x81")
    assert dec.main([str(b)]) == 2
