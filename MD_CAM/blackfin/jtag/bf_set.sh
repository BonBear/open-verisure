#!/bin/bash
# Write one 32-bit word into the running snap_link over JTAG: bf_set.sh <addr> <value>
# (the core is halted for the write and resumed by detach). Tuning words, see snap_link.c:
#   0xFF800034 = 0x80000000 | reg << 8 | value   one sensor register before the next picture
#   0xFF800030 = 0x5AA00000 | q                   sharpening (0 = off, default 4)
#   0xFF80003C = 0x51A70000 | shade_k << 8 | sat lens shading / saturation
#   0xFF800040 = 0x0A170000 | quality             JPEG quality
#   0xFF800050 = threshold                        delta threshold (0 = DELTA_T)
#   0xFF800078 = 0xF1000000 | frames << 16 | mask << 8 | t   flash test (t x 0.1 ms)
. "$(dirname "$0")/jtag_common.sh"
[ $# -eq 2 ] || { echo "usage: bf_set.sh <addr> <value>"; exit 1; }
proxy_start
timeout 60 $G -batch -ex "target remote localhost:2000" -ex "set *(unsigned int*)$1=$2" -ex "x/1xw $1" -ex detach build/snap_link.elf 2>&1 | grep -E "^0xff80|refused|rror"
proxy_stop
