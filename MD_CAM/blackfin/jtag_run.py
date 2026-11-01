#!/usr/bin/env python3
"""
jtag_run.py - Talk to the ADSP-BF512 on MD CAM board #1 over JTAG (J5) with a FT232H module.

Usage:
    python jtag_run.py detect             read the JTAG chain (IDCODE) with bfin-jtag (urJTAG)
    python jtag_run.py load [source.S]    build, load into L1 with bfin-gdbproxy + gdb, run to
                                          _done and read back the result (default: jtag_test.S)

Wiring FT232H -> J5: AD0=TCK (5), AD1=TDI (3), AD2=TDO (7), AD3=TMS (4), GND (6),
TRST (2) to 3.3 V through ~1 kOhm. The CC1110 must run the BF_JTAG_HOLD firmware so the
Blackfin is powered and out of reset.
"""
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLCHAIN = os.environ.get("BFIN_TOOLCHAIN", r"C:\Tools\adi-blackfin\elf\bin")
JTAG = os.path.join(TOOLCHAIN, "bfin-jtag.exe")
PROXY = os.path.join(TOOLCHAIN, "bfin-gdbproxy.exe")
GDB = os.path.join(TOOLCHAIN, "bfin-elf-gdb.exe")
NM = os.path.join(TOOLCHAIN, "bfin-elf-nm.exe")

CABLE = os.environ.get("BFIN_CABLE", "cable FT2232 vid=0403 pid=6014 driver=ftd2xx-mpsse")
FREQ = int(os.environ.get("BFIN_JTAG_HZ", "1000000"))
PORT = 2000
L1_DATA_A = 0xFF800000
CHIPID = 0xFFC00014


def detect() -> None:
    script = f"{CABLE}\nfrequency {FREQ}\ndetect\nquit\n"
    with tempfile.NamedTemporaryFile("w", suffix=".jtag", delete=False) as f:
        f.write(script)
        path = f.name
    try:
        res = subprocess.run([JTAG, "-q", path], capture_output=True, text=True, timeout=60)
    finally:
        os.remove(path)
    print(res.stdout + res.stderr)
    if "bf518" not in (res.stdout + res.stderr).lower():
        sys.exit("[-] No BF51x found in the chain: check J5 wiring, TRST high and that the Blackfin is powered")
    print("[+] BF51x found")


def symbol(elf: str, name: str) -> int:
    out = subprocess.run([NM, elf], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    sys.exit(f"[-] Symbol {name} not found in {elf}")


def load(source: str) -> None:
    subprocess.run([sys.executable, os.path.join(HERE, "build_ldr.py"), "--elf-only", source], cwd=HERE, check=True)
    elf = os.path.join(HERE, "build", os.path.splitext(os.path.basename(source))[0] + ".elf")
    done = symbol(elf, "_done")

    proxy = subprocess.Popen([PROXY, f"--port={PORT}", "bfin", f"--connect={CABLE}", f"--frequency={FREQ}"],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        time.sleep(3)
        if proxy.poll() is not None:
            sys.exit(f"[-] bfin-gdbproxy exited:\n{proxy.stdout.read()}")
        cmds = [
            f"target remote :{PORT}",
            "load",
            f"break *{done:#x}",
            "continue",
            f"x/2xw {L1_DATA_A:#x}",
            f"x/xw {CHIPID:#x}",
            "detach",
        ]
        args = [GDB, "-batch", "-nx"] + [a for c in cmds for a in ("-ex", c)] + [elf]
        res = subprocess.run(args, capture_output=True, text=True, timeout=120)
        out = res.stdout + res.stderr
        print(out)
    finally:
        proxy.terminate()
        try:
            print(proxy.communicate(timeout=5)[0])
        except subprocess.TimeoutExpired:
            proxy.kill()

    if "0xb1a5c0de" in out.lower():
        print("[+] Magic word found: our code ran on the Blackfin")
    else:
        sys.exit("[-] Magic word not found")


def main() -> None:
    if len(sys.argv) < 2 or sys.argv[1] not in ("detect", "load"):
        sys.exit(__doc__)
    if sys.argv[1] == "detect":
        detect()
    else:
        load(sys.argv[2] if len(sys.argv) > 2 else "jtag_test.S")


if __name__ == "__main__":
    main()
