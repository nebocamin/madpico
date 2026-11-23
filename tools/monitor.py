#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prints the serial output of the Pico, optionally sending commands.

Usage: monitor.py [--port /dev/ttyACM0] [--time 30] [--until TEXT] [--send s:5 ...]
  --send s:5   sends the character "s" after 5 seconds
"""
import argparse
import sys
import time

import serial

ap = argparse.ArgumentParser()
ap.add_argument("--port", default="/dev/ttyACM0")
ap.add_argument("--time", type=float, default=30, help="seconds to run")
ap.add_argument("--until", default=None, help="stop once this text was received")
ap.add_argument("--send", action="append", default=[], help="CHARS:SECONDS")
args = ap.parse_args()

sends = sorted((float(s.split(":", 1)[1]), s.split(":", 1)[0]) for s in args.send)
s = serial.Serial(args.port, 115200, timeout=0.2)
s.dtr = True
start = time.time()
buf = b""
try:
    while time.time() - start < args.time:
        while sends and time.time() - start >= sends[0][0]:
            s.write(sends.pop(0)[1].encode())
        d = s.read(4096)
        if d:
            sys.stdout.write(d.decode(errors="replace"))
            sys.stdout.flush()
            buf += d
            if args.until and args.until.encode() in buf and not sends:
                time.sleep(0.5)
                sys.stdout.write(s.read(4096).decode(errors="replace"))
                break
except serial.SerialException:
    # e.g. the Pico rebooted and the USB device disappeared
    print("\n[monitor] serial port disconnected", file=sys.stderr)
