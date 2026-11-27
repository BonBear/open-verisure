#!/usr/bin/env python3
"""Flash GW-SD3 firmware over SWD with pyOCD (CMSIS-DAP probe, e.g. Raspberry Pi Debug Probe).

Requires the Silicon Labs DFP pack once:  pyocd pack install EFR32FG14P231F256GM48
The chip must already be unlocked (AAP device erase); see ../README.md.

Usage: python tools/flash.py build/smoke.hex [--frequency 1000000]
"""
import argparse
import sys

from pyocd.core.helpers import ConnectHelper
from pyocd.flash.file_programmer import FileProgrammer

TARGET = "efr32fg14p231f256gm48"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image", help="firmware image (.hex/.elf/.bin)")
    ap.add_argument("--frequency", type=int, default=1_000_000, help="SWD clock in Hz")
    args = ap.parse_args()

    with ConnectHelper.session_with_chosen_probe(target_override=TARGET,
                                                 frequency=args.frequency) as session:
        FileProgrammer(session, chip_erase="sector").program(args.image)
        session.target.reset()
    print(f"Flashed {args.image} and reset the target")
    return 0


if __name__ == "__main__":
    sys.exit(main())
