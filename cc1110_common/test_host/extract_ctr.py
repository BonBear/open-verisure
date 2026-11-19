#!/usr/bin/env python3
"""Cut the persistent packet counter code out of a CC1110 main.c for the host tests.

The tests compile the real code, so nothing is copied by hand: this script writes, with #line
directives pointing back at main.c,
  - the WDT_FEED() macro, the dma_desc_t typedef and the dma_desc variable,
  - the region from '#define CTR_PAGE0' to the end of ctr_boot() (the log constants, ctr_buf,
    ctr_page, ctr_next, the #include of cc1110_common/ctr_log.c, ctr_pending and ctr_boot),
  - the counter rule run before every packet (the if / else if that logs a new block or retries
    ctr_pending), wrapped as  static void pkt_rule(uint32_t ctr, uint8_t boot_pkts).
It fails if the code no longer has the expected shape, or if main.c calls ctr_log() anywhere
else (the test model of when packets go out would then be incomplete).

Usage: extract_ctr.py <main.c> <output.inc>
"""

import os
import re
import sys


def die(msg):
    sys.exit("extract_ctr.py: " + msg)


def match_brace(text, open_pos):
    """Index just after the brace that closes text[open_pos] ('{'), skipping comments,
    string and character literals."""
    depth = 0
    i = open_pos
    n = len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            i = text.find("\n", i)
            if i < 0:
                break
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            if j < 0:
                die("unterminated comment")
            i = j + 2
            continue
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    die("no matching brace")


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def one(pattern, text, what, flags=re.MULTILINE):
    found = list(re.finditer(pattern, text, flags))
    if len(found) != 1:
        die("expected exactly one %s, found %d" % (what, len(found)))
    return found[0]


def main():
    if len(sys.argv) != 3:
        die("usage: extract_ctr.py <main.c> <output.inc>")
    src_path, out_path = sys.argv[1], sys.argv[2]
    with open(src_path, encoding="utf-8") as f:
        text = f.read()
    # #line names the file as given (relative to the Makefile's directory is fine for messages)
    src_name = src_path.replace("\\", "/")
    parts = []

    def chunk(start, end):
        parts.append('#line %d "%s"\n%s\n' % (line_of(text, start), src_name, text[start:end]))

    m = one(r"^#define WDT_FEED\(\).*$", text, "WDT_FEED() macro")
    chunk(m.start(), m.end())
    m = one(r"^typedef struct \{[^}]*\} dma_desc_t;", text, "dma_desc_t typedef")
    chunk(m.start(), m.end())
    m = one(r"^static __xdata dma_desc_t dma_desc;", text, "dma_desc variable")
    chunk(m.start(), m.end())

    start = one(r"^#define CTR_PAGE0\b", text, "#define CTR_PAGE0").start()
    sig = one(r"^static\s+\w+\s+ctr_boot\s*\(\s*void\s*\)\s*\{", text, "ctr_boot() definition")
    if sig.start() < start:
        die("ctr_boot() comes before #define CTR_PAGE0")
    region_end = match_brace(text, sig.end() - 1)
    region = text[start:region_end]
    if not re.search(r'^#include ".*cc1110_common/ctr_log\.c"', region, re.MULTILINE):
        die("the region does not include cc1110_common/ctr_log.c")
    chunk(start, region_end)

    # the counter rule before each packet: exactly four lines
    lines = text.split("\n")
    anchor = [i for i, l in enumerate(lines)
              if l.strip() == "ctr_pending = ctr_log(ctr >> 8) ? 0 : ctr >> 8;"]
    if len(anchor) != 1:
        die("expected one 'ctr_pending = ctr_log(ctr >> 8) ? 0 : ctr >> 8;', found %d"
            % len(anchor))
    a = anchor[0]
    rule = lines[a - 1:a + 3]
    if (not rule[0].strip().startswith("if (")
            or not rule[2].strip().startswith("else if (ctr_pending")
            or rule[3].strip() != "ctr_pending = 0;"
            or lines[a + 3].strip().startswith("else")):
        die("the counter rule before a packet does not have the expected shape:\n"
            + "\n".join(lines[a - 1:a + 4]))
    parts.append("static void pkt_rule(uint32_t ctr, uint8_t boot_pkts) {\n"
                 "    (void)ctr;\n    (void)boot_pkts;\n")
    parts.append('#line %d "%s"\n%s\n}\n' % (a, src_name, "\n".join(rule)))

    # every ctr_log() call must be in ctr_boot or in the rule above
    calls = [line_of(text, m.start()) for m in re.finditer(r"\bctr_log\s*\(", text)]
    region_lines = range(line_of(text, start), line_of(text, region_end) + 1)
    rule_lines = range(a, a + 4)
    stray = [n for n in calls if n not in region_lines and n not in rule_lines]
    if stray:
        die("ctr_log() is also called at line(s) %s of %s: extend the test model"
            % (", ".join(map(str, stray)), src_path))

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("/* Generated by extract_ctr.py from %s: do not edit. */\n" % src_name)
        f.write("".join(parts))


if __name__ == "__main__":
    main()
