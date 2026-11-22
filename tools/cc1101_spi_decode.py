#!/usr/bin/env python3
"""cc1101_spi_decode.py: decode a CC1101 SPI capture into strobes, register writes and settings.

    python tools/cc1101_spi_decode.py capture.csv
    python tools/cc1101_spi_decode.py capture.csv --xtal-mhz 27.0 --json

Input formats (auto-detected, CSV header names are case-insensitive, quotes optional):

1. Saleae Logic 2 SPI analyzer export (Analyzers > Export as CSV). Columns:
       name,type,start_time,duration,"mosi","miso"
   Rows of type `enable` start a transaction (CSn low), `result` rows carry one byte each
   (hex with the 0x prefix, e.g. 0x30, other values are skipped; the first one is the header byte) and `disable` rows end it (CSn high).
   Only the `type`, `mosi` and `miso` columns are needed (`start_time` is shown if present);
   other columns and `error` rows are ignored.
2. Plain text fallback: one transaction (one CSn low period) per line, the MOSI bytes in hex
   separated by spaces or commas, e.g. `40 07 2E 80`. A 0x prefix is allowed. Empty lines and
   lines starting with `#` are skipped. Reads cannot show MISO data in this format.

CC1101 SPI semantics (datasheet SWRS061): header byte = R/W (bit 7), burst (bit 6), address
(bits 5:0). Addresses 0x00-0x2E are configuration registers (burst auto-increments and does not
wrap), 0x30-0x3D with burst=0 are command strobes, with burst=1 (read) status registers, 0x3E is
the PATABLE and 0x3F the TX FIFO (write) / RX FIFO (read).

Output: a log of strobes and accesses, the final register image (last value written per
register) with the PATABLE, and settings derived from it (carrier, data rate, bandwidth,
deviation, sync word, packet format, WOR timing). Malformed input lines are skipped with a
warning on stderr. Standard library only.
"""
import argparse
import csv
import json
import re
import sys

REGS = [
    "IOCFG2", "IOCFG1", "IOCFG0", "FIFOTHR", "SYNC1", "SYNC0", "PKTLEN", "PKTCTRL1",
    "PKTCTRL0", "ADDR", "CHANNR", "FSCTRL1", "FSCTRL0", "FREQ2", "FREQ1", "FREQ0",
    "MDMCFG4", "MDMCFG3", "MDMCFG2", "MDMCFG1", "MDMCFG0", "DEVIATN", "MCSM2", "MCSM1",
    "MCSM0", "FOCCFG", "BSCFG", "AGCCTRL2", "AGCCTRL1", "AGCCTRL0", "WOREVT1", "WOREVT0",
    "WORCTRL", "FREND1", "FREND0", "FSCAL3", "FSCAL2", "FSCAL1", "FSCAL0", "RCCTRL1",
    "RCCTRL0", "FSTEST", "PTEST", "AGCTEST", "TEST2", "TEST1", "TEST0",
]  # 0x00..0x2E
REG_ADDR = {n: i for i, n in enumerate(REGS)}
STROBES = {
    0x30: "SRES", 0x31: "SFSTXON", 0x32: "SXOFF", 0x33: "SCAL", 0x34: "SRX", 0x35: "STX",
    0x36: "SIDLE", 0x37: "SAFC", 0x38: "SWOR", 0x39: "SPWD", 0x3A: "SFRX", 0x3B: "SFTX",
    0x3C: "SWORRST", 0x3D: "SNOP",
}
STATUS = {
    0x30: "PARTNUM", 0x31: "VERSION", 0x32: "FREQEST", 0x33: "LQI", 0x34: "RSSI",
    0x35: "MARCSTATE", 0x36: "WORTIME1", 0x37: "WORTIME0", 0x38: "PKTSTATUS",
    0x39: "VCO_VC_DAC", 0x3A: "TXBYTES", 0x3B: "RXBYTES", 0x3C: "RCCTRL1_STATUS",
    0x3D: "RCCTRL0_STATUS",
}
PATABLE_ADDR, FIFO_ADDR = 0x3E, 0x3F
LAST_CFG = 0x2E


class Txn:
    """One CSn-low transaction: MOSI and MISO byte lists and an optional start time."""

    def __init__(self, t=None):
        self.t = t
        self.mosi = []
        self.miso = []


# ------------------------------------------------------------------ parsing

def _byte(s, need_prefix=False):
    """Parse '0x30' / '30' into 0..255; '' gives None; anything else raises ValueError.
    need_prefix: Saleae values must carry 0x (a bare '10' may be decimal)."""
    s = s.strip().strip('"').strip()
    if not s:
        return None
    if not re.fullmatch(r"0[xX][0-9a-fA-F]{1,2}" if need_prefix else r"(0[xX])?[0-9a-fA-F]{1,2}", s):
        raise ValueError(s)
    return int(s, 16)


def parse_saleae(rows, hdr, warns):
    cols = [c.strip().strip('"').lower() for c in hdr]
    if "type" not in cols:
        warns.append("Saleae header has no 'type' column; nothing decoded")
        return []
    ti, mi = cols.index("type"), cols.index("mosi")
    so = cols.index("miso") if "miso" in cols else None
    st = cols.index("start_time") if "start_time" in cols else None
    txns, cur = [], None
    framed = False
    for n, r in rows:
        if len(r) <= max(ti, mi):
            warns.append(f"line {n}: too few columns, skipped")
            continue
        typ = r[ti].strip().strip('"').lower()
        t = r[st].strip() if st is not None and st < len(r) else None
        if typ == "enable":
            framed = True
            if cur is not None:
                txns.append(cur)
            cur = Txn(t)
        elif typ == "disable":
            framed = True
            if cur is not None:
                txns.append(cur)
            cur = None
        elif typ == "result":
            try:
                m = _byte(r[mi], True)
                s = _byte(r[so], True) if so is not None and so < len(r) else None
            except ValueError as e:
                warns.append(f"line {n}: bad byte value {e} (0x prefix required), skipped")
                continue
            if cur is None:  # result without a preceding enable row
                cur = Txn(t)
            cur.mosi.append(0 if m is None else m)
            cur.miso.append(s)  # None = no MISO value in the export
        else:
            warns.append(f"line {n}: row type '{typ}' ignored")
    if cur is not None:
        txns.append(cur)
    if txns and not framed:
        warns.append("no enable/disable rows: all result rows were taken as one transaction")
    return txns


def parse_text(text, warns):
    """Auto-detect the format and return (transactions); warnings are appended to warns."""
    lines = text.splitlines()
    first = next(((i, ln) for i, ln in enumerate(lines) if ln.strip()), None)
    if first is None:
        return []
    hdr = next(csv.reader([first[1]]))
    if "mosi" in [c.strip().strip('"').lower() for c in hdr]:
        body = []
        for i in range(first[0] + 1, len(lines)):
            if lines[i].strip():
                body.append((i + 1, next(csv.reader([lines[i]]))))
        return parse_saleae(body, hdr, warns)
    txns = []
    for i, ln in enumerate(lines, 1):
        s = ln.strip()
        if not s or s.startswith("#"):
            continue
        try:
            vals = [_byte(t) for t in re.split(r"[\s,;]+", s) if t]
        except ValueError as e:
            warns.append(f"line {i}: bad hex byte '{e}', line skipped")
            continue
        t = Txn()
        t.mosi = [v for v in vals if v is not None]
        txns.append(t)
    return txns


# ------------------------------------------------------------------ decoding

def hexs(bs):
    return " ".join(f"{b:02X}" for b in bs)


def decode(txns, warns):
    """Return (log lines, register image {addr: value}, PATABLE list)."""
    log, image, pa = [], {}, [None] * 8
    for k, t in enumerate(txns, 1):
        when = f"[{t.t}] " if t.t else ""
        if not t.mosi:
            warns.append(f"transaction {k}: empty, skipped")
            continue
        h = t.mosi[0]
        rd, burst, addr = bool(h & 0x80), bool(h & 0x40), h & 0x3F
        data = t.mosi[1:]
        rdata = t.miso[1:] if len(t.miso) > 1 and None not in t.miso[1:] else []
        if addr == 0x2F:
            warns.append(f"transaction {k}: no register at 0x2F (header 0x{h:02X}), skipped")
            continue
        if not burst and 0x30 <= addr <= 0x3D:
            name = STROBES[addr]
            log.append(f"{when}STROBE {name} (0x{addr:02X})" + (" [read bit set]" if rd else ""))
            if data:
                warns.append(f"transaction {k}: {len(data)} byte(s) after strobe {name} ignored")
        elif burst and 0x30 <= addr <= 0x3D:
            name = STATUS[addr]
            if not rd:
                log.append(f"{when}INVALID write to status register {name} (0x{addr:02X}), ignored")
                warns.append(f"transaction {k}: write to status register {name}")
            else:
                val = f" = {hexs(rdata)}" if rdata else ""
                log.append(f"{when}READ  status {name} (0x{addr:02X}){val}")
        elif addr == PATABLE_ADDR:
            if rd:
                log.append(f"{when}READ  PATABLE" + (f" = {hexs(rdata)}" if rdata else ""))
            else:
                bs = data if burst else data[:1]
                for i, b in enumerate(bs[:8]):
                    pa[i] = b
                if len(bs) > 8:
                    warns.append(f"transaction {k}: PATABLE write of {len(bs)} bytes, only 8 kept")
                if not bs:
                    warns.append(f"transaction {k}: PATABLE write without data")
                log.append(f"{when}WRITE PATABLE{' burst' if burst else ''}: {hexs(bs)}")
        elif addr == FIFO_ADDR:
            if rd:
                log.append(f"{when}READ  RX FIFO{' burst' if burst else ''}: {len(rdata)} byte(s)"
                           + (f" = {hexs(rdata)}" if rdata else ""))
            else:
                log.append(f"{when}WRITE TX FIFO{' burst' if burst else ''}: {len(data)} byte(s)"
                           f" = {hexs(data)}")
        else:  # configuration register 0x00..0x2E
            if rd:
                val = f" = {hexs(rdata)}" if rdata else ""
                log.append(f"{when}READ  {REGS[addr]} (0x{addr:02X}){' burst' if burst else ''}{val}")
                continue
            if not data:
                warns.append(f"transaction {k}: write to {REGS[addr]} has no data (truncated)")
                log.append(f"{when}WRITE {REGS[addr]} (0x{addr:02X}): no data (truncated)")
                continue
            used = data if burst else data[:1]
            if not burst and len(data) > 1:
                warns.append(f"transaction {k}: single write with {len(data)} bytes, extra ignored")
            parts = []
            for i, b in enumerate(used):
                a = addr + i
                if a > LAST_CFG:
                    warns.append(f"transaction {k}: burst write runs past 0x2E, "
                                 f"{len(used) - i} byte(s) ignored (no wrap)")
                    break
                image[a] = b
                parts.append(f"{REGS[a]}=0x{b:02X}")
            log.append(f"{when}WRITE {'burst ' if burst else ''}{REGS[addr]} (0x{addr:02X}): "
                       + ", ".join(parts))
    return log, image, pa


# ------------------------------------------------------------------ derived values

EVENT1 = (4, 6, 8, 12, 16, 24, 32, 48)
PREAMBLE = (2, 3, 4, 6, 8, 12, 16, 24)
MOD = {0: "2-FSK", 1: "GFSK", 3: "ASK/OOK", 4: "4-FSK", 7: "MSK"}
SYNC = ("no preamble/sync", "15/16 sync word bits", "16/16 sync word bits",
        "30/32 sync word bits", "no preamble/sync, carrier-sense above threshold",
        "15/16 + carrier-sense above threshold", "16/16 + carrier-sense above threshold",
        "30/32 + carrier-sense above threshold")
LENMODE = ("fixed length (PKTLEN)", "variable length (first byte, max PKTLEN)", "infinite length",
           "reserved")


def derive(img, xtal_mhz):
    """Settings computed from the final register image; items whose registers were never
    written are left out. Frequencies in Hz are returned as MHz/kHz floats."""
    fx = xtal_mhz * 1e6
    g = lambda n: img.get(REG_ADDR[n])  # noqa: E731
    have = lambda *ns: all(g(n) is not None for n in ns)  # noqa: E731
    d = {"xtal_mhz": xtal_mhz}
    if have("FREQ2", "FREQ1", "FREQ0"):
        freq = (g("FREQ2") << 16) | (g("FREQ1") << 8) | g("FREQ0")
        d["freq_word"] = freq
        d["carrier_mhz"] = fx / 2**16 * freq / 1e6
        if have("FSCTRL0"):
            off = g("FSCTRL0") - 256 if g("FSCTRL0") & 0x80 else g("FSCTRL0")  # signed 8 bit
            d["freq_offset_hz"] = fx / 2**14 * off
            d["carrier_with_offset_mhz"] = d["carrier_mhz"] + d["freq_offset_hz"] / 1e6
    if have("MDMCFG1", "MDMCFG0"):
        e, m = g("MDMCFG1") & 3, g("MDMCFG0")
        d["channel_spacing_khz"] = fx / 2**18 * (256 + m) * 2**e / 1e3
        d["fec"] = bool(g("MDMCFG1") & 0x80)
        d["preamble_bytes"] = PREAMBLE[(g("MDMCFG1") >> 4) & 7]
        if "carrier_mhz" in d and have("CHANNR") and g("CHANNR"):
            d["channel"] = g("CHANNR")
            d["channel_carrier_mhz"] = d["carrier_mhz"] + g("CHANNR") * d["channel_spacing_khz"] / 1e3
    if have("MDMCFG4", "MDMCFG3"):
        e, m = g("MDMCFG4") & 0x0F, g("MDMCFG3")
        d["data_rate_baud"] = (256 + m) * 2**e / 2**28 * fx
        ce, cm = (g("MDMCFG4") >> 6) & 3, (g("MDMCFG4") >> 4) & 3
        d["rx_filter_bw_khz"] = fx / (8 * (4 + cm) * 2**ce) / 1e3
    if have("DEVIATN"):
        e, m = (g("DEVIATN") >> 4) & 7, g("DEVIATN") & 7
        d["deviation_khz"] = fx / 2**17 * (8 + m) * 2**e / 1e3
    if have("MDMCFG2"):
        v = g("MDMCFG2")
        d["modulation"] = MOD.get((v >> 4) & 7, f"reserved ({(v >> 4) & 7})")
        d["manchester"] = bool(v & 8)
        d["sync_mode"] = SYNC[v & 7]
    if have("SYNC1", "SYNC0"):
        d["sync_word"] = f"0x{g('SYNC1'):02X}{g('SYNC0'):02X}"
    if have("PKTCTRL0"):
        v = g("PKTCTRL0")
        d["packet_length_mode"] = LENMODE[v & 3]
        d["crc_enabled"] = bool(v & 4)
        d["whitening"] = bool(v & 0x40)
    if have("PKTLEN"):
        d["pktlen"] = g("PKTLEN")
    if have("PKTCTRL1"):
        d["append_status"] = bool(g("PKTCTRL1") & 4)
        d["address_check"] = ("none", "no broadcast", "0x00 broadcast", "0x00/0xFF broadcast")[
            g("PKTCTRL1") & 3]
    if have("WOREVT1", "WOREVT0", "WORCTRL"):
        ev0 = (g("WOREVT1") << 8) | g("WOREVT0")
        res = g("WORCTRL") & 3
        d["wor"] = {
            "event0": ev0,
            "wor_res": res,
            "event0_timeout_ms": 750 / fx * ev0 * 2 ** (5 * res) * 1e3,
            "event1_clocks": EVENT1[(g("WORCTRL") >> 4) & 7],
            "event1_ms": 750 / fx * EVENT1[(g("WORCTRL") >> 4) & 7] * 1e3,
            "rc_cal": bool(g("WORCTRL") & 8),
        }
        if have("MCSM2"):
            rx = g("MCSM2") & 7
            d["wor"]["rx_time_code"] = rx if rx != 7 else "7 (until end of packet)"
            d["wor"]["rx_time_rssi"] = bool(g("MCSM2") & 0x10)
            d["wor"]["rx_time_qual"] = bool(g("MCSM2") & 0x08)
    return d


def fmt_derived(d):
    out = []
    add = lambda s: out.append("  " + s)  # noqa: E731
    add(f"crystal            {d['xtal_mhz']:g} MHz")
    if "carrier_mhz" in d:
        add(f"carrier            {d['carrier_mhz']:.4f} MHz (FREQ word {d['freq_word']} = 0x{d['freq_word']:06X})")
    if "carrier_with_offset_mhz" in d:
        add(f"carrier + FSCTRL0   {d['carrier_with_offset_mhz']:.6f} MHz (offset {d['freq_offset_hz']:+.0f} Hz)")
    if "channel_carrier_mhz" in d:
        add(f"channel {d['channel']:<10} {d['channel_carrier_mhz']:.4f} MHz")
    if "channel_spacing_khz" in d:
        add(f"channel spacing    {d['channel_spacing_khz']:.3f} kHz")
    if "data_rate_baud" in d:
        add(f"data rate          {d['data_rate_baud']:.1f} Baud")
        add(f"RX filter BW       {d['rx_filter_bw_khz']:.1f} kHz")
    if "deviation_khz" in d:
        add(f"deviation          {d['deviation_khz']:.2f} kHz")
    if "modulation" in d:
        add(f"modulation         {d['modulation']}{', Manchester' if d['manchester'] else ''}")
        add(f"sync mode          {d['sync_mode']}")
    if "sync_word" in d:
        add(f"sync word          {d['sync_word']}")
    if "preamble_bytes" in d:
        add(f"preamble           {d['preamble_bytes']} bytes{', FEC on' if d['fec'] else ''}")
    if "packet_length_mode" in d:
        add(f"packet length      {d['packet_length_mode']}" + (f", PKTLEN={d['pktlen']}" if "pktlen" in d else ""))
        add(f"CRC                {'on' if d['crc_enabled'] else 'off'}")
        add(f"whitening          {'on' if d['whitening'] else 'off'}")
    if "address_check" in d:
        add(f"address check      {d['address_check']}, append status {'on' if d['append_status'] else 'off'}")
    if "wor" in d:
        w = d["wor"]
        add(f"WOR EVENT0         {w['event0']} (WOR_RES {w['wor_res']}) -> timeout {w['event0_timeout_ms']:.1f} ms")
        add(f"WOR EVENT1         {w['event1_clocks']} clocks = {w['event1_ms']:.3f} ms, RC_CAL {'on' if w['rc_cal'] else 'off'}")
        if "rx_time_code" in w:
            add(f"WOR RX_TIME        {w['rx_time_code']}, RSSI {'on' if w['rx_time_rssi'] else 'off'}, "
                f"QUAL {'on' if w['rx_time_qual'] else 'off'}")
    return out


# ------------------------------------------------------------------ main

def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", help="Saleae Logic 2 SPI CSV export, or text with one hex transaction per line")
    ap.add_argument("--xtal-mhz", type=float, default=26.0,
                    help="CC1101 crystal in MHz (default 26.0; the Verisure boards use 27.0)")
    ap.add_argument("--json", action="store_true", help="print the image and derived values as JSON")
    a = ap.parse_args(argv)
    if not a.xtal_mhz > 0:
        ap.error("--xtal-mhz must be greater than 0")
    try:
        with open(a.capture, "rb") as f:
            raw = f.read()
        if raw.startswith((b"\xff\xfe", b"\xfe\xff")):
            text = raw.decode("utf-16")
        else:
            text = raw.decode("utf-8-sig")
    except OSError as e:
        print(f"[-] cannot read {a.capture}: {e}", file=sys.stderr)
        return 2
    except UnicodeDecodeError as e:
        print(f"[-] {a.capture} is not UTF-8 or UTF-16 text ({e}); export the capture as CSV text",
              file=sys.stderr)
        return 2
    warns = []
    txns = parse_text(text, warns)
    log, img, pa = decode(txns, warns)
    der = derive(img, a.xtal_mhz)
    for w in warns:
        print(f"[!] {w}", file=sys.stderr)
    if a.json:
        print(json.dumps({
            "transactions": len(txns),
            "registers": {REGS[k]: f"0x{v:02X}" for k, v in sorted(img.items())},
            "patable": [None if b is None else f"0x{b:02X}" for b in pa],
            "derived": der,
            "warnings": warns,
        }, indent=2))
        return 0
    print(f"== Log ({len(txns)} transactions)")
    for ln in log:
        print(ln)
    print("\n== Final register image (last value written)")
    for k, name in enumerate(REGS):
        v = img.get(k)
        print(f"  0x{k:02X} {name:<9} " + ("--" if v is None else f"0x{v:02X}"))
    print("  PATABLE   " + " ".join("--" if b is None else f"{b:02X}" for b in pa))
    print("\n== Derived settings")
    for ln in fmt_derived(der):
        print(ln)
    return 0


if __name__ == "__main__":
    sys.exit(main())
