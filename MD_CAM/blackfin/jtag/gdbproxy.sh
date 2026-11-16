#!/bin/bash
# Start bfin-gdbproxy on port 2000 against the FT232H attached to WSL (run as root in WSL,
# started by jtag_common.sh). ftdi_sio grabs the FT232H whenever it is free, so release it
# right before starting. BFIN_BIN overrides where the ADI toolchain's bfin-gdbproxy is.
BIN=${BFIN_BIN:-$(ls -d /home/*/adi-blackfin/opt/uClinux/bfin-elf/bin 2>/dev/null | head -1)}
for i in /sys/bus/usb/drivers/ftdi_sio/*:*; do
    [ -e "$i" ] && basename "$i" > /sys/bus/usb/drivers/ftdi_sio/unbind
done
rmmod ftdi_sio 2>/dev/null
exec "$BIN/bfin-gdbproxy" --port=2000 bfin --connect="cable FT2232 vid=0x0403 pid=0x6014" --frequency=1000000 --wait-clocks=30
