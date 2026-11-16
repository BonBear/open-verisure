#!/bin/bash
# Shared part of the Blackfin JTAG scripts (Git Bash on Windows, FT232H in WSL).
#
# Before use (once per Windows session; the WSL VM stops when nothing runs in it):
#   powershell Start-Process wsl -ArgumentList '-u','root','sleep','28800'   # keeps WSL up 8 h
#   usbipd attach --wsl --busid <FT232H busid>                              # e.g. 5-4
# The Blackfin must be powered (the CC1110 powers it for a picture and keeps it on 120 s).
# Run the scripts with MSYS_NO_PATHCONV=1 so Git Bash leaves the gdb arguments alone.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BLACKFIN=$(cd "$HERE/.." && pwd)
G=${BFIN_GDB:-/c/Tools/adi-blackfin/elf/bin/bfin-elf-gdb.exe}
LOG=$HERE/gdbproxy.log
WSL_HERE=/mnt/$(echo "${HERE:1:1}" | tr 'A-Z' 'a-z')${HERE:2}

proxy_start() {
    wsl -u root pkill -f bfin-gdbproxy 2>/dev/null
    powershell -Command "Start-Process -WindowStyle Hidden wsl -ArgumentList '-u','root','bash','-c','\"bash $WSL_HERE/gdbproxy.sh > $WSL_HERE/gdbproxy.log 2>&1\"'"
    python -c "import time; time.sleep(12)"
    wsl -u root sh -c 'pgrep -f bfin-gdbproxy >/dev/null' || { echo "gdbproxy did not start"; tail -5 "$LOG"; exit 1; }
}

proxy_stop() {
    wsl -u root pkill -f bfin-gdbproxy 2>/dev/null
}

cd "$BLACKFIN" || exit 1
