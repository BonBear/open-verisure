#!/bin/bash
# Write build/snap_link.ldr to the Blackfin boot flash at 0x8000 over JTAG (flash_write.elf
# erases only sectors inside 0x8000..0xFFFF and verifies). flash_write takes at most 0x4000
# bytes per run (its data buffer is L1 bank B at 0xFF900000), so a larger LDR is written in
# 16 KB parts; each part must report magic 0xF1A5D07E and result 0 before the next (one retry).
# The CC1110 keeps the Blackfin on only 15 s after a picture: publish HOLD first
# (verisure/<id>/picture/set HOLD keeps it on 10 min without a picture).
# Build first: python build_ldr.py snap_link.c jpeg_enc.c   (in MD_CAM/blackfin)
. "$(dirname "$0")/jtag_common.sh"
N=$(stat -c %s build/snap_link.ldr)
echo "LDR $N bytes"
[ "$N" -le 32768 ] || { echo "LDR larger than the 32 KB area"; exit 1; }
proxy_start
OFF=0
while [ $OFF -lt $N ]; do
  LEN=$((N - OFF)); [ $LEN -gt 16384 ] && LEN=16384
  ADDR=$(printf "0x%x" $((0x8000 + OFF)))
  BIAS=$(printf "0x%x" $((0xff900000 - OFF)))
  for TRY in 1 2; do
  echo "part at $ADDR, $LEN bytes (try $TRY)"
  timeout 120 $G -batch -ex "target remote localhost:2000" \
    -ex "set *(unsigned short*)0xffc01000=0" -ex "set *(unsigned short*)0xffc00c08=0" -ex "set *(unsigned short*)0xffc00500=0" \
    -ex "set *(int*)0xffe00004=0x00001001" -ex "load build/flash_write.elf" \
    -ex "restore build/snap_link.ldr binary $BIAS $OFF $((OFF + LEN))" \
    -ex "set *(int*)0xff800010=$ADDR" -ex "set *(int*)0xff800014=$LEN" -ex "set *(int*)0xff80001c=0xE7A5E000" \
    -ex detach build/flash_write.elf 2>&1 | grep -vE "^$|no debugging"
  python -c "import time; time.sleep(30)"
  R=$(timeout 60 $G -batch -ex "target remote localhost:2000" -ex "x/8xw 0xff800000" -ex detach build/flash_write.elf 2>&1 | grep -E "^0xff80|rror")
  echo "$R"
  echo "$R" | grep -q "^0xff800000:.0xf1a5d07e.0x00000000" && break
  # seen on 2026-11-16: the first run right after snap_link had booted sometimes failed
  # (result 2, nothing erased); the second run always worked. flash_write verifies everything.
  [ $TRY -eq 2 ] && { echo "part at $ADDR FAILED"; proxy_stop; exit 1; }
  done
  OFF=$((OFF + LEN))
done
echo "all parts written and verified"
proxy_stop
