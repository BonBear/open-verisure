#!/bin/bash
# Read snap_link's status words and the delta histogram (0xFF800000..0xFF8000FC) over JTAG.
# The core is halted for the read and resumed by detach. See snap_link.c for the layout.
. "$(dirname "$0")/jtag_common.sh"
proxy_start
timeout 60 $G -batch -ex "target remote localhost:2000" -ex "x/64xw 0xff800000" -ex detach build/snap_link.elf 2>&1 | grep -E "^0xff80|refused"
proxy_stop
