#!/usr/bin/env python3
"""
monitor.py - Real-Time Serial Monitor for Verisure 868 Gateway

Listens to the ESP32-S2 gateway on USB-CDC serial (115200 baud) and formats
incoming telemetry packets, Home Assistant discovery events, and heartbeats.

Features:
- Automatic COM port discovery with interactive selection menu if port is omitted.
- Automatic hardware identification (ESP32-S2 Native USB vs CP210x Flasher).
- Supports both continuous monitoring (default or duration=0) and timed sessions.

Usage Examples:
    # 1. Interactive selection (lists available ports):
    python monitor.py

    # 2. Specify port directly:
    python monitor.py --port COM9

    # 3. Specify duration (e.g. 60 seconds) or continuous (0):
    python monitor.py -p COM9 -d 60
    python monitor.py 60 COM9
"""

import sys
import os
import time
import argparse
import serial
from port_selector import select_com_port

BAUD = 115200


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Monitor Verisure Gateway Serial Output",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    parser.add_argument("--port", "-p", default=None,
                        help="Serial COM port (e.g. COM9, /dev/ttyACM0 on Linux). If omitted, interactive selector is shown.")
    parser.add_argument("--duration", "-d", type=int, default=None,
                        help="Monitoring duration in seconds (default: 0 = continuous until Ctrl+C).")
    parser.add_argument("--baud", "-b", type=int, default=BAUD,
                        help=f"Serial baud rate (default: {BAUD}).")

    # Positional backwards compatibility: monitor.py [duration] [port]
    parser.add_argument("pos_args", nargs="*", help="Optional positional [duration] [port]")

    args = parser.parse_args()

    # Handle legacy positional arguments if provided
    port = args.port or os.environ.get("RECEIVER_PORT")
    duration = args.duration

    if args.pos_args:
        # First positional argument can be duration
        if len(args.pos_args) >= 1:
            try:
                duration = int(args.pos_args[0])
            except ValueError:
                port = args.pos_args[0]
        # Second positional argument can be port
        if len(args.pos_args) >= 2:
            port = args.pos_args[1]

    if duration is None:
        duration = 0  # Default to continuous

    return port, duration, args.baud


def main():
    port_arg, duration, baud = parse_arguments()

    # If no port specified, or if requested port is not available, prompt user
    port = select_com_port(preferred_port=port_arg, hint_keyword="Gateway", prompt_title="Select Gateway Serial Port")
    if not port:
        sys.exit(1)

    dur_str = f"{duration} seconds" if duration > 0 else "until Ctrl+C is pressed"
    print("=" * 65)
    print(f"  MONITORING VERISURE GATEWAY ON {port} ({dur_str})")
    print("  Trigger a device (door sensor: magnet, shock, tamper; camera: PIR) to see its packets.")
    print("=" * 65 + "\n")

    try:
        ser = serial.Serial(port, baud, timeout=0.1)
    except Exception as e:
        print(f"[-] Failed to open {port}: {e}")
        sys.exit(1)

    t0 = time.time()
    try:
        while True:
            if duration > 0 and (time.time() - t0 >= duration):
                break
            line = ser.readline().decode('utf-8', errors='replace').strip()
            if line:
                print(line)
    except KeyboardInterrupt:
        print("\nCancelled by user.")
    finally:
        ser.close()
        print("\n" + "=" * 65)
        print("  Monitoring finished.")
        print("=" * 65)


if __name__ == "__main__":
    main()
