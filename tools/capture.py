#!/usr/bin/env python3
"""Capture M5CAN Phase-1 text output to an append-only log."""

from __future__ import annotations

import argparse
import datetime as dt
from pathlib import Path

import serial
import serial.tools.list_ports


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    if args.list:
        for port in serial.tools.list_ports.comports():
            print(f"{port.device}\t{port.description}")
        return 0

    if not args.port:
        parser.error("--port is required unless --list is used")

    output = args.output
    if output is None:
        stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
        output = Path("logs") / f"m5can-{stamp}.log"
    output.parent.mkdir(parents=True, exist_ok=True)

    with serial.Serial(args.port, args.baud, timeout=1) as device, output.open("ab") as log:
        print(f"capturing {args.port} -> {output}")
        while True:
            line = device.readline()
            if not line:
                continue
            log.write(line)
            log.flush()
            print(line.decode("utf-8", errors="replace"), end="")


if __name__ == "__main__":
    raise SystemExit(main())
