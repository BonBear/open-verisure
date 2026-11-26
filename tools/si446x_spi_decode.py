#!/usr/bin/env python3
"""si446x_spi_decode.py: decode a Si4460/Si446x (EZRadioPRO) SPI capture into commands and settings.

    python tools/si446x_spi_decode.py capture.csv
    python tools/si446x_spi_decode.py capture.csv --xtal-mhz 30.0 --json --verbose

Input formats (auto-detected, CSV header names are case-insensitive, quotes optional):

1. Saleae Logic 2 SPI analyzer export (Analyzers > Export as CSV). Columns:
       name,type,start_time,duration,"mosi","miso"
   Rows of type `enable` start a transaction (nSEL low), `result` rows carry one byte each
   (hex with the 0x prefix, e.g. 0x31, other values are skipped; the first one is the command byte)
   and `disable` rows end it (nSEL high). Only the `type`, `mosi` and `miso` columns are needed
   (`start_time` is shown if present); other columns and `error` rows are ignored.
2. Plain text fallback: one transaction (one nSEL low period) per line, the MOSI bytes in hex
   separated by spaces or commas, e.g. `11 20 01 51 08`. A 0x prefix is allowed. Empty lines and
   lines starting with `#` are skipped. Replies cannot be shown in this format (no MISO).

Si446x SPI semantics: a command-based API, not registers. Each nSEL-low transaction starts with a
command ID on MOSI followed by its arguments. A reply is fetched afterwards with READ_CMD_BUFF
(0x44): MISO byte 1 is CTS (0xFF = ready, anything else = not ready), MISO bytes 2.. are the reply
of the most recent command that has one. FRR reads (0x50/0x51/0x53/0x57) return data directly from
MISO byte 1 (no CTS). WRITE_TX_FIFO (0x66) carries data bytes, READ_RX_FIFO (0x77) returns them on
MISO bytes 1...

Output: a log of commands (SET_PROPERTY with property names, replies paired with their request),
the final property image (last value written per property), the TX/RX FIFO payloads and settings
derived from the image (carrier, channel step, deviation, data rate, modulation, sync, preamble,
CRC). Malformed input lines are skipped with a warning on stderr. Standard library only.
"""
import argparse
import csv
import json
import re
import sys

CMDS = {
    0x00: "NOP", 0x01: "PART_INFO", 0x02: "POWER_UP", 0x10: "FUNC_INFO", 0x11: "SET_PROPERTY",
    0x12: "GET_PROPERTY", 0x13: "GPIO_PIN_CFG", 0x14: "GET_ADC_READING", 0x15: "FIFO_INFO",
    0x16: "PACKET_INFO", 0x17: "IRCAL", 0x1A: "IRCAL_MANUAL", 0x20: "GET_INT_STATUS",
    0x21: "GET_PH_STATUS", 0x22: "GET_MODEM_STATUS", 0x23: "GET_CHIP_STATUS", 0x31: "START_TX",
    0x32: "START_RX", 0x33: "REQUEST_DEVICE_STATE", 0x34: "CHANGE_STATE", 0x36: "RX_HOP",
    0x37: "TX_HOP", 0x44: "READ_CMD_BUFF", 0x50: "FRR_A_READ", 0x51: "FRR_B_READ",
    0x53: "FRR_C_READ", 0x57: "FRR_D_READ", 0x66: "WRITE_TX_FIFO", 0x77: "READ_RX_FIFO",
}
FRR_CMDS = (0x50, 0x51, 0x53, 0x57)
# Commands whose reply is fetched with READ_CMD_BUFF
REPLY_CMDS = (0x01, 0x10, 0x12, 0x14, 0x15, 0x16, 0x20, 0x21, 0x22, 0x23, 0x33)
STATES = {0: "NOCHANGE", 1: "SLEEP", 2: "SPI_ACTIVE", 3: "READY", 4: "READY2", 5: "TX_TUNE",
          6: "RX_TUNE", 7: "TX", 8: "RX"}
REPLY_NAMES = {
    0x20: ("INT_PEND", "INT_STATUS", "PH_PEND", "PH_STATUS", "MODEM_PEND", "MODEM_STATUS",
           "CHIP_PEND", "CHIP_STATUS"),
    0x21: ("PH_PEND", "PH_STATUS"),
    0x22: ("MODEM_PEND", "MODEM_STATUS", "CURR_RSSI", "LATCH_RSSI", "ANT1_RSSI", "ANT2_RSSI"),
    0x23: ("CHIP_PEND", "CHIP_STATUS", "CMD_ERR_STATUS"),
    0x15: ("RX_FIFO_COUNT", "TX_FIFO_SPACE"),
}
GPIO_NAMES = ("GPIO0", "GPIO1", "GPIO2", "GPIO3", "NIRQ", "SDO", "GEN_CONFIG")

GROUP_NAMES = {
    0x00: "GLOBAL", 0x01: "INT_CTL", 0x02: "FRR_CTL", 0x10: "PREAMBLE", 0x11: "SYNC", 0x12: "PKT",
    0x20: "MODEM", 0x21: "MODEM_CHFLT", 0x22: "PA", 0x23: "SYNTH", 0x30: "MATCH",
    0x40: "FREQ_CONTROL", 0x50: "RX_HOP",
}
FIELDS = {}  # (group, prop) -> (full name, first prop, last prop) of the field containing it


def _add(group, items):
    for it in items:
        lo = it[1]
        hi = it[2] if len(it) > 2 else lo
        f = (f"{GROUP_NAMES[group]}_{it[0]}", lo, hi)
        for p in range(lo, hi + 1):
            FIELDS[(group, p)] = f


def _build_fields():
    _add(0x00, [("XO_TUNE", 0), ("CLK_CFG", 1), ("LOW_BATT_THRESH", 2), ("CONFIG", 3),
                ("WUT_CONFIG", 4), ("WUT_M", 5, 6), ("WUT_R", 7), ("WUT_LDC", 8), ("WUT_CAL", 9)])
    _add(0x01, [("ENABLE", 0), ("PH_ENABLE", 1), ("MODEM_ENABLE", 2), ("CHIP_ENABLE", 3)])
    _add(0x02, [(f"{c}_MODE", i) for i, c in enumerate("ABCD")])
    _add(0x10, [("TX_LENGTH", 0), ("CONFIG_STD_1", 1), ("CONFIG_NSTD", 2), ("CONFIG_STD_2", 3),
                ("CONFIG", 4), ("PATTERN", 5, 8), ("POSTAMBLE_CONFIG", 9),
                ("POSTAMBLE_PATTERN", 0x0A, 0x0D)])
    _add(0x11, [("CONFIG", 0), ("BITS", 1, 4), ("CONFIG2", 5)])
    pkt = [("CRC_CONFIG", 0), ("WHT_POLY", 1, 2), ("WHT_SEED", 3, 4), ("WHT_BIT_NUM", 5),
           ("CONFIG1", 6), ("CONFIG2", 7), ("LEN", 8), ("LEN_FIELD_SOURCE", 9),
           ("LEN_ADJUST", 0x0A), ("TX_THRESHOLD", 0x0B), ("RX_THRESHOLD", 0x0C),
           ("CRC_SEED", 0x36, 0x39)]
    for i in range(5):
        for pre, base in (("FIELD", 0x0D), ("RX_FIELD", 0x21)):
            b = base + 4 * i
            pkt += [(f"{pre}_{i + 1}_LENGTH", b, b + 1), (f"{pre}_{i + 1}_CONFIG", b + 2),
                    (f"{pre}_{i + 1}_CRC_CONFIG", b + 3)]
    _add(0x12, pkt)
    modem = [("MOD_TYPE", 0), ("MAP_CONTROL", 1), ("DSM_CTRL", 2), ("DATA_RATE", 3, 5),
             ("TX_NCO_MODE", 6, 9), ("FREQ_DEV", 0x0A, 0x0C), ("FREQ_OFFSET", 0x0D, 0x0E),
             ("IF_CONTROL", 0x1A), ("IF_FREQ", 0x1B, 0x1D), ("CLKGEN_BAND", 0x51)]
    modem += [(f"TX_FILTER_COEFF_{8 - i}", 0x0F + i) for i in range(9)]
    _add(0x20, modem)
    _add(0x21, [("RX1_CHFLT_COE", 0, 0x11), ("RX2_CHFLT_COE", 0x12, 0x23)])
    _add(0x22, [("MODE", 0), ("PWR_LVL", 1), ("BIAS_CLKDUTY", 2), ("TC", 3), ("RAMP_EX", 4),
                ("RAMP_DOWN_DELAY", 5), ("DIG_PWR_SEQ_CONFIG", 6)])
    _add(0x23, [("PFDCP_CPFF", 0), ("PFDCP_CPINT", 1), ("VCO_KV", 2), ("LPFILT3", 3),
                ("LPFILT2", 4), ("LPFILT1", 5), ("LPFILT0", 6), ("VCO_KVCAL", 7)])
    _add(0x30, [(f"{n}_{i}", 3 * (i - 1) + k) for i in range(1, 5)
                for k, n in enumerate(("VALUE", "MASK", "CTRL"))])
    _add(0x40, [("INTE", 0), ("FRAC", 1, 3), ("CHANNEL_STEP_SIZE", 4, 5), ("W_SIZE", 6),
                ("VCOCNT_RX_ADJ", 7)])
    _add(0x50, [("CONTROL", 0), ("TABLE_SIZE", 1)] + [(f"TABLE_ENTRY_{n}", 2 + n) for n in range(64)])


_build_fields()


def prop_field(g, p):
    f = FIELDS.get((g, p))
    if f:
        return f
    if g == 0x20:
        return (f"MODEM_0x{p:02X}", p, p)
    return (f"GROUP_0x{g:02X}_0x{p:02X}", p, p)


def segments(g, start, count):
    """Split props start..start+count-1 of group g into runs of one named field:
    returns [(name, first prop, last prop)] covering only the props actually in the range."""
    out = []
    for p in range(start, min(start + count, 0x100)):
        f = prop_field(g, p)
        if out and out[-1][3] == f:
            out[-1][2] = p
        else:
            out.append([f[0], p, p, f])
    return [(n, lo, hi) for n, lo, hi, _ in out]


def rng(g, lo, hi):
    return f"0x{g:02X}{lo:02X}" + (f"..0x{g:02X}{hi:02X}" if hi != lo else "")


class Txn:
    """One nSEL-low transaction: MOSI and MISO byte lists and an optional start time."""

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


class Result:
    def __init__(self, verbose=False):
        self.verbose = verbose
        self.log = []
        self.when = ""
        self.image = {}          # (group, prop) -> last written byte
        self.fxo = None          # Hz, from POWER_UP
        self.part = None
        self.pending = None      # (cmd id, request bytes) awaiting a READ_CMD_BUFF reply
        self.tx_packets = []
        self.rx_packets = []
        self.polls = 0           # READ_CMD_BUFF polls with CTS != 0xFF

    def add(self, s):
        self.log.append(self.when + s)


def _named(names, rep):
    parts = [f"{n}=0x{b:02X}" for n, b in zip(names, rep)]
    if len(rep) > len(names):
        parts.append("extra=" + hexs(rep[len(names):]))
    return ", ".join(parts)


def decode_reply(r, req, rep, warns):
    """Log the reply `rep` of the pending request `req` (request bytes incl. command ID)."""
    cid = req[0]
    name = CMDS[cid]
    if cid == 0x01:
        if len(rep) < 8:
            r.add(f"REPLY {name}: short ({len(rep)} bytes) = {hexs(rep)}")
            return
        r.part = f"0x{(rep[1] << 8) | rep[2]:04X}"
        r.add(f"REPLY {name}: CHIPREV=0x{rep[0]:02X}, PART={r.part}, PBUILD=0x{rep[3]:02X}, "
              f"ID=0x{(rep[4] << 8) | rep[5]:04X}, CUSTOMER=0x{rep[6]:02X}, ROMID=0x{rep[7]:02X}")
    elif cid == 0x10:
        if len(rep) < 6:
            r.add(f"REPLY {name}: short ({len(rep)} bytes) = {hexs(rep)}")
            return
        r.add(f"REPLY {name}: REVEXT=0x{rep[0]:02X}, REVBRANCH=0x{rep[1]:02X}, "
              f"REVINT=0x{rep[2]:02X}, PATCH=0x{(rep[3] << 8) | rep[4]:04X}, FUNC=0x{rep[5]:02X}")
    elif cid == 0x33:
        st = rep[0] & 0x0F if rep else None
        r.add(f"REPLY {name}: " + (f"state {STATES.get(st, f'0x{st:X}')}, CURRENT_CHANNEL="
                                   f"0x{rep[1]:02X}" if len(rep) > 1 else hexs(rep)))
    elif cid == 0x12:
        g, num, start = req[1:4] if len(req) >= 4 else (0, 0, 0)
        if len(rep) < num:
            warns.append(f"GET_PROPERTY reply has {len(rep)} byte(s), {num} requested")
        for n, lo, hi in segments(g, start, min(num, len(rep))):
            r.add(f"GET reply {n} ({rng(g, lo, hi)}) = {hexs(rep[lo - start:hi - start + 1])}")
    elif cid in REPLY_NAMES:
        r.add(f"REPLY {name}: {_named(REPLY_NAMES[cid], rep)}")
    else:
        r.add(f"REPLY {name}: {hexs(rep)}")


def decode_txn(k, t, r, warns):
    m = t.mosi
    cid = m[0]
    name = CMDS.get(cid)
    rdata = t.miso[1:] if len(t.miso) > 1 and None not in t.miso[1:] else []
    if cid == 0x44:  # READ_CMD_BUFF
        if len(t.miso) < 2 or t.miso[1] is None:
            r.add("READ_CMD_BUFF: no MISO, CTS unknown")
            return
        cts = t.miso[1]
        rep = t.miso[2:]
        if cts != 0xFF:
            r.polls += 1
            if r.verbose:
                r.add(f"READ_CMD_BUFF: CTS=0x{cts:02X} (not ready)")
        elif None in rep or not rep:
            if r.verbose:
                r.add("READ_CMD_BUFF: CTS=0xFF (ready, no reply bytes)")
        elif r.pending is not None:
            req, r.pending = r.pending[1], None
            decode_reply(r, req, rep, warns)
        else:
            r.add(f"READ_CMD_BUFF: CTS=0xFF, reply without pending command = {hexs(rep)}")
    elif cid in FRR_CMDS:
        r.add(f"READ {name} (0x{cid:02X})" + (f" = {hexs(rdata)}" if rdata else ""))
    elif cid == 0x66:  # WRITE_TX_FIFO: over-the-air payload
        data = m[1:]
        if data:
            r.tx_packets.append(hexs(data))
        r.add(f"WRITE_TX_FIFO: {len(data)} byte(s) = {hexs(data)}   <== TX payload")
    elif cid == 0x77:  # READ_RX_FIFO: over-the-air payload
        if rdata:
            r.rx_packets.append(hexs(rdata))
            r.add(f"READ_RX_FIFO: {len(rdata)} byte(s) = {hexs(rdata)}   <== RX payload")
        elif len(m) == 1 and len(t.miso) == 1 and t.miso[0] is not None:
            r.add("READ_RX_FIFO: 0 byte(s)")
        else:
            r.add("READ_RX_FIFO: byte count unknown (no MISO)")
    elif name is None:
        r.add(f"UNKNOWN 0x{cid:02X}" + (f" args {hexs(m[1:])}" if len(m) > 1 else ""))
    else:
        decode_cmd(k, name, m, r, warns)
        if cid in REPLY_CMDS:
            r.pending = (cid, list(m))


def decode_cmd(k, name, m, r, warns):
    cid = m[0]

    def short(n):
        if len(m) < n:
            warns.append(f"transaction {k}: {name} has {len(m) - 1} arg byte(s), expected {n - 1}")
            return True
        return False

    if cid == 0x02:  # POWER_UP
        if short(7):
            r.add(f"POWER_UP (truncated): {hexs(m[1:])}")
            return
        xo = int.from_bytes(bytes(m[3:7]), "big")
        r.fxo = xo if xo > 0 else r.fxo
        r.add(f"POWER_UP BOOT_OPTIONS=0x{m[1]:02X} (PATCH={m[1] >> 7}, FUNC={m[1] & 0x3F}), "
              f"XTAL_OPTIONS=0x{m[2]:02X} (TCXO={m[2] & 1}), XO_FREQ={xo} Hz")
    elif cid == 0x11:  # SET_PROPERTY
        if short(4):
            r.add(f"SET_PROPERTY (truncated): {hexs(m[1:])}")
            return
        g, num, start, data = m[1], m[2], m[3], m[4:]
        if len(data) != num:
            warns.append(f"transaction {k}: SET_PROPERTY NUM_PROPS={num} but {len(data)} data byte(s)")
            data = data[:num]  # the radio ignores bytes beyond NUM_PROPS
        for i, b in enumerate(data):
            if start + i > 0xFF:
                warns.append(f"transaction {k}: SET_PROPERTY runs past property 0xFF, rest ignored")
                break
            r.image[(g, start + i)] = b
        for n, lo, hi in segments(g, start, len(data)):
            r.add(f"SET {n} ({rng(g, lo, hi)}) = {hexs(data[lo - start:hi - start + 1])}")
    elif cid == 0x12:  # GET_PROPERTY
        if short(4):
            r.add(f"GET_PROPERTY (truncated): {hexs(m[1:])}")
            return
        for n, lo, hi in segments(m[1], m[3], m[2]):
            r.add(f"GET {n} ({rng(m[1], lo, hi)})")
    elif cid == 0x13:  # GPIO_PIN_CFG
        parts = []
        for i, b in enumerate(m[1:8]):
            nm = GPIO_NAMES[i]
            parts.append(f"{nm}=0x{b:02X}" + (f" (mode {b & 0x3F}, pull {(b >> 6) & 1})" if i < 6 else ""))
        r.add("GPIO_PIN_CFG " + ", ".join(parts))
    elif cid == 0x34:  # CHANGE_STATE
        s = m[1] if len(m) > 1 else None
        r.add("CHANGE_STATE " + ("(no arg)" if s is None else f"-> {STATES.get(s, f'0x{s:02X}')}"))
    elif cid == 0x31:  # START_TX
        if short(5):
            r.add(f"START_TX (truncated): {hexs(m[1:])}")
            return
        r.add(f"START_TX CHANNEL={m[1]}, CONDITION=0x{m[2]:02X} "
              f"(TXCOMPLETE_STATE={STATES.get(m[2] >> 4, m[2] >> 4)}), "
              f"TX_LEN={((m[3] << 8) | m[4]) & 0x1FFF}")
    elif cid == 0x32:  # START_RX; layout after TX_LEN is unverified (taken from API docs)
        a = m[1:]
        st = lambda i: STATES.get(a[i], f"0x{a[i]:02X}")
        s = f"START_RX CHANNEL={a[0]}" if a else "START_RX"
        if len(a) >= 2:
            s += f", CONDITION=0x{a[1]:02X}"
        if len(a) >= 4:
            s += f", RX_LEN={((a[2] << 8) | a[3]) & 0x1FFF}"
        if len(a) >= 7:
            s += f", RXTIMEOUT_STATE={st(4)}, RXVALID_STATE={st(5)}, RXINVALID_STATE={st(6)}"
        r.add(s)
    else:
        r.add(f"{name}" + (f" args {hexs(m[1:])}" if len(m) > 1 else ""))


def decode(txns, warns, verbose=False):
    """Return a Result with log lines, property image, packets and the CTS poll count."""
    r = Result(verbose)
    for k, t in enumerate(txns, 1):
        r.when = f"[{t.t}] " if t.t else ""
        if not t.mosi:
            warns.append(f"transaction {k}: empty, skipped")
            continue
        decode_txn(k, t, r, warns)
    return r


# ------------------------------------------------------------------ derived values

OUTDIV = {0: 4, 1: 6, 2: 8, 3: 12, 4: 16, 5: 24, 6: 24, 7: 24}
TXOSR = {0: 10, 1: 40, 2: 20}
MODTYPE = {0: "CW", 1: "OOK", 2: "2FSK", 3: "2GFSK", 4: "4FSK", 5: "4GFSK"}
MODSRC = {0: "PACKET", 1: "DIRECT", 2: "PN9"}
CRCS = {0: "none", 1: "ITU-T CRC8", 2: "IEC-16", 3: "Baicheva-16", 4: "CRC-16 IBM",
        5: "CCITT-16", 6: "Koopman", 7: "IEEE 802.3", 8: "Castagnoli", 9: "CRC-16 DNP"}


def derive(img, fxo, part, warns):
    """Settings computed from the final property image; items whose properties were never
    written are left out. All formulas are per Si446x API docs; verify against WDS."""
    d = {"xtal_mhz": fxo / 1e6}
    if part:
        d["part"] = part

    def word(g, p, n):
        bs = [img.get((g, p + i)) for i in range(n)]
        return None if None in bs else int.from_bytes(bytes(bs), "big")

    band = img.get((0x20, 0x51))
    k = None
    if band is not None:
        # per Si446x API docs; verify against WDS: outdiv from CLKGEN_BAND bits 2:0, NPRESC from SY_SEL
        d["outdiv"] = OUTDIV[band & 7]
        d["nprescaler"] = 2 if band & 8 else 4
        k = d["nprescaler"] * fxo / d["outdiv"]
    inte, frac = img.get((0x40, 0)), word(0x40, 1, 3)
    if k and inte is not None and frac is not None:
        # per Si446x API docs; verify against WDS: fc = (INTE + FRAC/2^19) * NPRESC * fxo / outdiv
        frac &= 0x0FFFFF
        if not 1 <= frac / 2**19 < 2:
            warns.append(f"FREQ_CONTROL_FRAC/2^19 = {frac / 2**19:.4f} is outside [1, 2)")
        d["inte"], d["frac"] = inte, frac
        d["carrier_hz"] = (inte + frac / 2**19) * k
    step = word(0x40, 4, 2)
    if k and step is not None:
        d["channel_step_hz"] = step / 2**19 * k  # per Si446x API docs; verify against WDS
    dev = word(0x20, 0x0A, 3)
    if k and dev is not None:
        d["deviation_hz"] = (dev & 0x1FFFF) * k / 2**19  # per Si446x API docs; verify against WDS
    dr, nco = word(0x20, 3, 3), word(0x20, 6, 4)
    if dr is not None and nco is not None:
        # per Si446x API docs; verify against WDS: rate = DATA_RATE * fxo / NCOMOD / TXOSR factor
        code = (nco >> 24 >> 2) & 3
        if code not in TXOSR:
            warns.append("TX_NCO_MODE TXOSR code 3 is reserved; data rate not derived")
        elif nco & 0x03FFFFFF:
            d["data_rate_bps"] = dr * fxo / (nco & 0x03FFFFFF) / TXOSR[code]
    mt = img.get((0x20, 0))
    if mt is not None:
        d["modulation"] = MODTYPE.get(mt & 7, f"reserved ({mt & 7})")
        d["mod_source"] = MODSRC.get((mt >> 3) & 3, f"reserved ({(mt >> 3) & 3})")
    sc = img.get((0x11, 0))
    if sc is not None:
        n = (sc & 3) + 1
        d["sync_length_bytes"] = n
        d["sync_skip_tx"] = bool(sc & 0x80)
        bs = [img.get((0x11, 1 + i)) for i in range(n)]
        if None not in bs:
            d["sync_word"] = hexs(bs)  # as written; Si446x sends byte 3 first, each byte LSB first
    if (0x10, 0) in img:
        d["preamble_tx_length"] = img[(0x10, 0)]
    if (0x10, 4) in img:
        d["preamble_config"] = f"0x{img[(0x10, 4)]:02X}"
    cc = img.get((0x12, 0))
    if cc is not None:
        d["crc"] = CRCS.get(cc & 15, f"reserved ({cc & 15})")
        d["crc_seed_all_ones"] = bool(cc & 0x80)
    c1 = img.get((0x12, 6))
    if c1 is not None:  # bit positions inferred from the API docs
        d["bit_order"] = "LSB first" if c1 & 1 else "MSB first"
        d["crc_endian"] = "MSB first" if c1 & 2 else "LSB first"
        d["fsk4"] = bool(c1 & 0x20)
        d["field_split"] = bool(c1 & 0x80)
    if (0x22, 1) in img:
        d["pa_pwr_lvl"] = f"0x{img[(0x22, 1)]:02X}"
    if (0x00, 0) in img:
        d["xo_tune"] = f"0x{img[(0x00, 0)]:02X}"
    return d


def fmt_derived(d):
    out = []
    add = lambda s: out.append("  " + s)
    add(f"crystal            {d['xtal_mhz']:g} MHz")
    if "part" in d:
        add(f"part number        Si{int(d['part'], 16):X} ({d['part']})")
    if "outdiv" in d:
        add(f"outdiv / NPRESC   {d['outdiv']} / {d['nprescaler']}")
    if "carrier_hz" in d:
        add(f"carrier            {d['carrier_hz'] / 1e6:.6f} MHz (INTE {d['inte']}, FRAC 0x{d['frac']:05X})")
    if "channel_step_hz" in d:
        add(f"channel step       {d['channel_step_hz']:.1f} Hz")
    if "deviation_hz" in d:
        add(f"deviation          {d['deviation_hz']:.1f} Hz")
    if "data_rate_bps" in d:
        add(f"data rate          {d['data_rate_bps']:.1f} bps")
    if "modulation" in d:
        add(f"modulation         {d['modulation']}, source {d['mod_source']}")
    if "sync_length_bytes" in d:
        add(f"sync               {d['sync_length_bytes']} byte(s)" + (", SKIP_TX" if d["sync_skip_tx"] else "")
            + (f", word {d['sync_word']}" if "sync_word" in d else ""))
        if "sync_word" in d:
            add("                   (Si446x sends byte 3 first, each byte LSB first)")
    if "preamble_tx_length" in d:
        add(f"preamble           TX_LENGTH {d['preamble_tx_length']}"
            + (f", CONFIG {d['preamble_config']}" if "preamble_config" in d else ""))
    elif "preamble_config" in d:
        add(f"preamble           CONFIG {d['preamble_config']}")
    if "crc" in d:
        add(f"CRC                {d['crc']}" + (", seed all ones" if d["crc_seed_all_ones"] else ""))
    if "bit_order" in d:
        add(f"packet bits        {d['bit_order']}, CRC {d['crc_endian']}, 4FSK {'on' if d['fsk4'] else 'off'}, "
            f"field split {'on' if d['field_split'] else 'off'} (positions inferred)")
    if "pa_pwr_lvl" in d:
        add(f"PA_PWR_LVL         {d['pa_pwr_lvl']}")
    if "xo_tune" in d:
        add(f"XO_TUNE            {d['xo_tune']}")
    return out


# ------------------------------------------------------------------ main

def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", help="Saleae Logic 2 SPI CSV export, or text with one hex transaction per line")
    ap.add_argument("--xtal-mhz", type=float, default=None,
                    help="crystal in MHz (default: XO_FREQ from POWER_UP, else 30.0)")
    ap.add_argument("--json", action="store_true", help="print the image and derived values as JSON")
    ap.add_argument("--verbose", action="store_true", help="also log READ_CMD_BUFF polls with CTS != 0xFF")
    a = ap.parse_args(argv)
    if a.xtal_mhz is not None and not a.xtal_mhz > 0:
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
    r = decode(txns, warns, a.verbose)
    if a.xtal_mhz is not None:
        fxo = a.xtal_mhz * 1e6
    elif r.fxo:
        fxo = float(r.fxo)
    else:
        fxo = 30e6
        if r.image:
            warns.append("no POWER_UP seen and --xtal-mhz not given: assuming 30.0 MHz")
    der = derive(r.image, fxo, r.part, warns)
    for w in warns:
        print(f"[!] {w}", file=sys.stderr)
    if a.json:
        print(json.dumps({
            "transactions": len(txns),
            "properties": {
                f"0x{g:02X}{p:02X}": {"name": prop_field(g, p)[0], "value": f"0x{v:02X}"}
                for (g, p), v in sorted(r.image.items())},
            "tx_packets": r.tx_packets,
            "rx_packets": r.rx_packets,
            "cts_not_ready_polls": r.polls,
            "derived": der,
            "warnings": warns,
        }, indent=2))
        return 0
    print(f"== Log ({len(txns)} transactions)")
    for ln in r.log:
        print(ln)
    if r.polls and not a.verbose:
        print(f"({r.polls} READ_CMD_BUFF poll(s) with CTS != 0xFF not shown; use --verbose)")
    print("\n== Final property image (last value written)")
    last = None
    for (g, p), v in sorted(r.image.items()):
        if g != last:
            print(f"  [{GROUP_NAMES.get(g, 'GROUP')} 0x{g:02X}]")
            last = g
        print(f"    0x{g:02X}{p:02X} {prop_field(g, p)[0]:<28} 0x{v:02X}")
    print("\n== TX/RX payloads")
    for i, pk in enumerate(r.tx_packets, 1):
        print(f"  TX #{i} ({len(pk.split())} bytes): {pk}")
    for i, pk in enumerate(r.rx_packets, 1):
        print(f"  RX #{i} ({len(pk.split())} bytes): {pk}")
    print("\n== Derived settings")
    for ln in fmt_derived(der):
        print(ln)
    return 0


if __name__ == "__main__":
    sys.exit(main())
