#!/usr/bin/env python3
"""Cut the persistent packet counter code out of a CC1110 main.c for the host tests.

The tests compile the real code, so nothing is copied by hand: this script writes, with #line
directives pointing back at main.c,
  - the WDT_FEED() macro, the dma_desc_t typedef and the dma_desc variable,
  - the region from '#define CTR_PAGE0' to the end of ctr_boot() (the log constants, ctr_buf,
    ctr_page, ctr_next, the #include of cc1110_common/ctr_log.c and ctr_boot),
  - the counter rule run before every packet (the one call ctr_use(ctr >> 8), whose result says
    whether the packet is sent), wrapped as  static bool pkt_rule(uint32_t ctr),
  - in the camera's main.c, the picture path from '#define BF_IMG_CHUNK' to the end of
    bf_send_picture() (with pic_ctr_ok()), after an #include of pic_stubs.h.
It fails if the code no longer has the expected shape, or if main.c calls ctr_log() or
ctr_use() anywhere else (the test model of when packets go out would then be incomplete). The
one exception is ctr_use(s_ctr >> 8) in the camera's pic_ctr_ok(): the check before a picture
that its start packet will be sent (the same rule on the same counter, which transmit() then
runs again). In the camera's main.c it also checks what pic_stubs.h assumes of code the tests
do not compile: transmit() sets s_tx_held from the rule, cam_poll() returns at once after a
held poll, and every bf_send_picture() call is guarded by pic_ok (pic_ctr_ok()).

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


def body_of(text, name):
    """(start, end) of the body of the one top-level definition of the static function name()."""
    defs = list(re.finditer(r"^static\s+[\w\s]*?\b%s\s*\([^)]*\)\s*\{" % name, text,
                            re.MULTILINE))
    if len(defs) != 1:
        die("expected one definition of %s(), found %d" % (name, len(defs)))
    return defs[0].end(), match_brace(text, defs[0].end() - 1)


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

    # the camera's picture path: '#define BF_IMG_CHUNK' to the end of bf_send_picture(), after
    # the stubs of what it calls (pic_stubs.h)
    pic = list(re.finditer(r"^static\s+bool\s+bf_send_picture\s*\([^)]*\)\s*\{", text,
                           re.MULTILINE))
    if pic:
        if len(pic) != 1:
            die("expected at most one bf_send_picture() definition")
        pstart = one(r"^#define BF_IMG_CHUNK\b", text, "#define BF_IMG_CHUNK").start()
        if not (region_end < pstart < pic[0].start()):
            die("#define BF_IMG_CHUNK is not between ctr_boot() and bf_send_picture()")
        parts.append('#include "pic_stubs.h"\n')
        chunk(pstart, match_brace(text, pic[0].end() - 1))

    # the counter rule before each packet: one call of ctr_use() with the packet's block
    rule = one(r"\bctr_use\s*\(\s*ctr\s*>>\s*8\s*\)", text, "call 'ctr_use(ctr >> 8)'")
    parts.append('static bool pkt_rule(uint32_t ctr) {\n#line %d "%s"\n    return %s;\n}\n'
                 % (line_of(text, rule.start()), src_name, rule.group(0)))

    # the camera: the tests model transmit(), cam_poll() and the alarm loop with stubs
    # (pic_stubs.h), so check that the real code still does what those stubs assume
    exempt = None
    if pic:
        tx = body_of(text, "transmit")
        if not tx[0] < rule.start() < tx[1]:
            die("the rule 'ctr_use(ctr >> 8)' is not in transmit()")
        if not re.search(r"\bs_tx_held\s*=\s*!\s*ctr_use\s*\(\s*ctr\s*>>\s*8\s*\)\s*;",
                         text[tx[0]:tx[1]]):
            die("transmit() no longer sets 's_tx_held = !ctr_use(ctr >> 8);' (pic_stubs.h)")
        poll = body_of(text, "cam_poll")
        if not re.search(r"\btransmit\s*\([^;]*\);\s*if\s*\(\s*s_tx_held\s*\|\|",
                         text[poll[0]:poll[1]]):
            die("cam_poll() no longer returns at once after a held poll ('if (s_tx_held ||')")
        calls = [m for m in re.finditer(r"\bbf_send_picture\s*\(", text)
                 if not re.search(r"\bbool\s*$", text[:m.start()])]
        if not calls or any(not re.search(r"\bpic_ok\s*&&\s*$", text[:m.start()])
                            for m in calls):
            die("every bf_send_picture() call must be guarded by 'pic_ok &&' (pic_ctr_ok())")
        ok = body_of(text, "pic_ctr_ok")
        m = re.search(r"\bctr_use\s*\(\s*s_ctr\s*>>\s*8\s*\)", text[ok[0]:ok[1]])
        if not m:
            die("pic_ctr_ok() no longer calls ctr_use(s_ctr >> 8)")
        exempt = ok[0] + m.start()

    # every ctr_log() call must be in ctr_boot, and ctr_use() only the call above (and, on the
    # camera, the one in pic_ctr_ok())
    region_lines = range(line_of(text, start), line_of(text, region_end) + 1)
    stray = [line_of(text, m.start()) for m in re.finditer(r"\bctr_log\s*\(", text)
             if line_of(text, m.start()) not in region_lines]
    stray += [line_of(text, m.start()) for m in re.finditer(r"\bctr_use\s*\(", text)
              if m.start() not in (rule.start(), exempt)]
    if stray:
        die("ctr_log()/ctr_use() is also called at line(s) %s of %s: extend the test model"
            % (", ".join(map(str, sorted(stray))), src_path))

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("/* Generated by extract_ctr.py from %s: do not edit. */\n" % src_name)
        f.write("".join(parts))


if __name__ == "__main__":
    main()
