#!/bin/bash
# Read part of the Blackfin boot flash to a file over JTAG (flash_read.elf, read-only):
#   bf_read_flash.sh <flash address> <length> <output file>
# Read in pieces of 16 KB (flash_read's buffer is L1 bank B at 0xFF900000). The Blackfin must
# be powered: publish HOLD to verisure/<id>/picture/set first.
. "$(dirname "$0")/jtag_common.sh"
[ $# -eq 3 ] || { echo "usage: bf_read_flash.sh <addr> <len> <file>"; exit 1; }
ADDR=$(($1)); LEN=$(($2)); OUT=$3
case "$OUT" in /*|[A-Za-z]:*) ;; *) OUT="$OLDPWD/$OUT" ;; esac
: > "$OUT"
proxy_start
OFF=0
while [ $OFF -lt $LEN ]; do
  N=$((LEN - OFF)); [ $N -gt 16384 ] && N=16384
  A=$(printf "0x%x" $((ADDR + OFF)))
  timeout 120 $G -batch -ex "target remote localhost:2000" \
    -ex "set *(unsigned short*)0xffc01000=0" -ex "set *(unsigned short*)0xffc00c08=0" -ex "set *(unsigned short*)0xffc00500=0" \
    -ex "set *(int*)0xffe00004=0x00001001" -ex "load build/flash_read.elf" \
    -ex "set *(int*)0xff800010=1" -ex "set *(int*)0xff800014=$A" -ex "set *(int*)0xff800018=$N" \
    -ex detach build/flash_read.elf 2>&1 | grep -E "rror|refused"
  python -c "import time; time.sleep(15)"
  T=build/_flash_read_part.bin      # relative: gdb is a Windows program
  # read as hex words (gdb's binary "dump" failed with this gdbproxy: "invalid hex digit")
  # and convert; capture the output first (grep -q would kill gdb early through the pipe)
  XS=(); P=0
  while [ $P -lt $N ]; do
    XS+=(-ex "x/256xw $(printf "0x%x" $((0xff900000 + P)))")
    P=$((P + 1024))
  done
  R=$(timeout 300 $G -batch -ex "target remote localhost:2000" -ex "x/1xw 0xff800000" \
    "${XS[@]}" -ex detach build/flash_read.elf 2>&1)
  echo "$R" | grep -q "0xff800000:.0xf1a50d0e" || { echo "read at $A FAILED"; echo "$R" | tail -3; proxy_stop; exit 1; }
  echo "$R" | python -c "
import sys, re, struct
d = b''.join(struct.pack('<I', int(w, 16)) for l in sys.stdin
             if re.match(r'0xff9[0-9a-f]{5}:', l) for w in l.split(':', 1)[1].split())
open(sys.argv[1], 'wb').write(d[:int(sys.argv[2])])" "$T" "$N"
  [ "$(stat -c %s "$T")" -eq "$N" ] || { echo "read at $A: short read"; proxy_stop; rm -f "$T"; exit 1; }
  cat "$T" >> "$OUT"; rm -f "$T"
  echo "read $A, $N bytes"
  OFF=$((OFF + N))
done
proxy_stop
