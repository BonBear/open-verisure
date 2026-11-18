#!/usr/bin/env python3
"""
port_selector.py - Interactive Serial COM Port Discovery & Selection Utility

Provides automatic detection and an interactive selection prompt for COM ports
when multiple USB serial devices are connected. Identifies common microcontroller
USB chips (Espressif ESP32 native USB, Silicon Labs CP210x, WCH CH340, FTDI).

Usage:
    # As a module:
    from port_selector import select_com_port
    port = select_com_port(preferred_port="COM9", hint_keyword="Gateway")   # or "/dev/ttyACM0"
    port = find_usb_port(GATEWAY_USB)                                      # by USB ID, any OS

    # Standalone CLI test:
    python port_selector.py
"""

import sys
from typing import Optional, List
import serial.tools.list_ports
from serial.tools.list_ports_common import ListPortInfo


def get_available_ports() -> List[ListPortInfo]:
    """
    Scan and return all active serial COM ports on the system.

    Returns:
        List[ListPortInfo]: List of serial port objects sorted by device name.
    """
    ports = list(serial.tools.list_ports.comports())
    ports.sort(key=lambda p: p.device)
    return ports


def get_device_hint(port: ListPortInfo) -> str:
    """
    Generate a human-friendly description and identification hint for a COM port.

    Args:
        port (ListPortInfo): Serial port object.

    Returns:
        str: Helpful hint (e.g. 'Espressif ESP32-S2 Gateway', 'CP210x CC-Flasher').
    """
    vid = port.vid
    pid = port.pid
    desc = port.description or ""

    # Espressif Systems native USB (ESP32-S2 / ESP32-S3 / ESP32-C3 CDC)
    if vid == 0x303A:
        if pid == 0x80C2:
            return "ESP32-S2 Native CDC (Verisure Gateway)"
        elif pid == 0x1001:
            return "ESP32-S3 Native CDC"
        return "Espressif USB Serial Device"

    # Silicon Labs CP210x (Commonly used on ESP32 development boards / CC-Flasher)
    if vid == 0x10C4 and pid == 0xEA60:
        return "Silicon Labs CP210x (ESP32 CC-Flasher)"

    # WCH CH340 / CH341
    if vid == 0x1A86:
        return "WCH CH340 USB-UART"

    # FTDI
    if vid == 0x0403:
        return "FTDI USB-UART"

    # Fallback to standard description
    return desc if desc else "Serial Port"


GATEWAY_USB = (0x303A, 0x80C2)   # ESP32-S2 native USB (the gateway)
FLASHER_USB = (0x10C4, 0xEA60)   # CP210x on the ESP32 CC flasher board


def find_usb_port(vid_pid) -> Optional[str]:
    """
    Return the first serial port with this USB VID/PID, on any OS (e.g. 'COM9' on Windows,
    '/dev/ttyACM0' on Linux), or None if none is connected.
    """
    for p in get_available_ports():
        if (p.vid, p.pid) == tuple(vid_pid):
            return p.device
    return None


def select_com_port(
    preferred_port: Optional[str] = None,
    hint_keyword: Optional[str] = None,
    prompt_title: str = "Select COM Port"
) -> Optional[str]:
    """
    Select an active COM port. If no port is specified or if multiple ports are
    found, presents an interactive menu to the user.

    Args:
        preferred_port (str, optional): Pre-selected port (e.g. from CLI flag or env).
        hint_keyword (str, optional): Keyword to prefer (e.g. 'Gateway' or 'ESP32').
        prompt_title (str): Header text displayed above the selection menu.

    Returns:
        Optional[str]: Selected port name (e.g. 'COM9') or None if no ports exist.
    """
    ports = get_available_ports()

    # 1. No ports detected
    if not ports:
        print("\n[-] No active COM ports found on system!")
        print("    Ensure the ESP32 gateway or CC-Flasher is connected via USB.\n")
        return None

    # Check if preferred_port exists in active ports
    if preferred_port:
        clean_pref = preferred_port.strip().upper()
        for p in ports:
            if p.device.upper() == clean_pref:
                return p.device
        # Preferred port was specified, but is not connected
        print(f"\n[!] Warning: Specified port '{preferred_port}' was not found.")
        print("    Listing available ports instead:\n")

    # 2. Exactly one port detected
    if len(ports) == 1:
        single_port = ports[0]
        hint = get_device_hint(single_port)
        print(f"[+] Found 1 active COM port: {single_port.device} ({hint}) - using automatically.")
        return single_port.device

    # 3. Multiple ports detected -> Determine default recommendation
    default_idx = 0
    if hint_keyword:
        kw = hint_keyword.lower()
        for i, p in enumerate(ports):
            hint = get_device_hint(p).lower()
            if kw in hint or kw in p.description.lower() or kw in p.device.lower():
                default_idx = i
                break

    # If running in a non-interactive environment (CI / pipe / subagent), choose default
    if not sys.stdin.isatty():
        chosen = ports[default_idx]
        print(f"[i] Non-interactive session: automatically choosing {chosen.device} ({get_device_hint(chosen)}).")
        return chosen.device

    # Interactive Menu
    print("=" * 65)
    print(f"  {prompt_title.upper()}")
    print("=" * 65)
    print(f"Found {len(ports)} connected COM port(s):\n")

    for i, p in enumerate(ports):
        hint = get_device_hint(p)
        rec_tag = " (Recommended)" if i == default_idx else ""
        print(f"  [{i + 1}] {p.device:<7} - {hint}{rec_tag}")

    print("-" * 65)
    default_num = default_idx + 1

    while True:
        try:
            choice = input(f"Select port [1-{len(ports)}] or enter port name (default: {default_num}): ").strip()
        except (KeyboardInterrupt, EOFError):
            print("\nCancelled by user.")
            sys.exit(130)

        # User pressed Enter without typing -> accept default
        if not choice:
            selected = ports[default_idx].device
            print(f"[+] Selected: {selected} ({get_device_hint(ports[default_idx])})\n")
            return selected

        # User entered a numeric index
        if choice.isdigit():
            val = int(choice)
            if 1 <= val <= len(ports):
                selected = ports[val - 1].device
                print(f"[+] Selected: {selected} ({get_device_hint(ports[val - 1])})\n")
                return selected
            else:
                print(f"[-] Invalid choice! Enter a number between 1 and {len(ports)}.")
                continue

        # User entered the port name directly (e.g. "COM9" or "com9")
        choice_upper = choice.upper()
        for p in ports:
            if p.device.upper() == choice_upper:
                print(f"[+] Selected: {p.device} ({get_device_hint(p)})\n")
                return p.device

        print(f"[-] Port '{choice}' was not recognized. Please try again.")


def main():
    print("Testing port_selector...")
    chosen = select_com_port(hint_keyword="Gateway")
    if chosen:
        print(f"Result: Selected port is {chosen}")
    else:
        print("No port could be selected.")


if __name__ == "__main__":
    main()
