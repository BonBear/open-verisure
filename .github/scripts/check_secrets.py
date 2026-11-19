#!/usr/bin/env python3
"""Key guard for CI: fail if a tracked file holds an AES key that is not a known public one.

The project's master key once ended up in a public document (TODO 0). GitHub's secret scanning
does not know this key format, so every push and pull request is checked here:

* no generated secret file is tracked (868Receiver/src/secrets.h, any config.h);
* every 32-hex-digit string is one of the public example or test values below;
* every 16-byte C array (0x.., 0x.., ...) near the word KEY is one of them too.

A finding prints the file and line, never the value. To add a new public test value, put it in
ALLOWED with a comment saying where it comes from.
"""
import re
import subprocess
import sys

ALLOWED = {
    "0102030405060708090A0B0C0D0E0F10",  # example master key (secrets.h.example, gateway fallback)
    "000102030405060708090A0B0C0D0E0F",  # CI test master key and MD_CAM/firmware/config.h.example
    "7960FF23E9DFE83C57BF94B437CCC0B9",  # device key of 2K79A65A under the CI test master key
    "00112233445566778899AABBCCDDEEFF",  # derive_key.py usage example
    "2B7E151628AED2A6ABF7158809CF4F3C",  # FIPS-197 AES test key (VerisureDoorSensor/config.h.example)
}
SKIP_EXT = (".jpg", ".jpeg", ".png", ".gif", ".pdf", ".bin", ".ldr", ".elf", ".zip")
SECRET_FILES = re.compile(r"(^|/)(secrets\.h|config\.h)$")
HEX32 = re.compile(r"(?<![0-9A-Fa-f])[0-9A-Fa-f]{32}(?![0-9A-Fa-f])")
BYTE = re.compile(r"0[xX]([0-9A-Fa-f]{2})")


def main() -> int:
    files = subprocess.run(["git", "ls-files", "-z"], capture_output=True, text=True,
                           check=True).stdout.split("\0")
    bad = []
    for f in filter(None, files):
        if SECRET_FILES.search(f):
            bad.append(f"{f}: generated secret file is tracked (it must stay git-ignored)")
            continue
        if f.lower().endswith(SKIP_EXT):
            continue
        try:
            lines = open(f, encoding="utf-8", errors="replace").read().split("\n")
        except OSError:
            continue
        for i, line in enumerate(lines, 1):
            for m in HEX32.finditer(line):
                if m.group(0).upper() not in ALLOWED:
                    bad.append(f"{f}:{i}: 32-hex-digit value that is not a known public example")
        # 16-byte arrays: the bytes of up to 4 lines after a line that mentions KEY
        for i, line in enumerate(lines):
            if "KEY" not in line.upper():
                continue
            vals = []
            for j in range(i, min(i + 5, len(lines))):
                vals += BYTE.findall(lines[j])
            for k in range(0, len(vals) - 15):
                key = "".join(vals[k:k + 16]).upper()
                if key not in ALLOWED:
                    bad.append(f"{f}:{i + 1}: 16-byte array near 'KEY' that is not a known public example")
                    break
    for b in sorted(set(bad)):
        print(f"::error::{b}")
    print(f"key guard: {len(files) - 1} tracked files, {len(set(bad))} findings")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
