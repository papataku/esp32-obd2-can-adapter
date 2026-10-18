#!/usr/bin/env python3
"""Strict OFFLINE importer for can-utils `candump -L` Classical CAN captures.

Example source:
  (1609770956.314100) can0 18DA02F1#0322201200000000
  (1609770956.314110) can0 18DAF102#100A622012010203
  (1609770956.314120) can0 123#R8

This command NEVER opens a CAN interface or transmits messages. It converts
a preexisting text file to the canonical independent CAN evidence format.
The original capture SHA-256 is included in the converted metadata, but the
capture and configuration still require independent operator verification.
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

from validate_multi_ecu_bench_trace import EvidenceError

LINE = re.compile(
    r"^\((\d+)\.(\d{1,6})\)\s+(\S+)\s+"
    r"([0-9a-fA-F]{3}|[0-9a-fA-F]{8})#([0-9a-fA-F]{0,16}|R[0-8]?)$"
)


def convert(source, *, interface, lost_frames):
    if not isinstance(interface, str) or not interface or any(c.isspace() for c in interface):
        raise EvidenceError("an exact source CAN interface name is required")
    if type(lost_frames) is not int or lost_frames < 0:
        raise EvidenceError("explicit nonnegative independent analyzer loss counter required")
    try:
        input_lines = source.decode("utf-8").splitlines()
    except UnicodeError as exc:
        raise EvidenceError("candump source is not valid UTF-8") from exc

    first_time = None
    last_time = -1
    frames = []
    for number, line in enumerate(input_lines, 1):
        if not line.strip():
            continue
        m = LINE.fullmatch(line.strip())
        if not m:
            raise EvidenceError(f"candump line {number}: unsupported/invalid format")
        seconds, fraction, channel, id_hex, content = m.groups()
        if channel != interface:
            raise EvidenceError(f"candump line {number}: interface changed from {interface}")
        can_id = int(id_hex, 16)
        extended = len(id_hex) == 8
        if can_id > (0x1FFFFFFF if extended else 0x7FF):
            raise EvidenceError(f"candump line {number}: out-of-range arbitration ID")
        raw_time = int(seconds) * 1_000_000 + int(fraction.ljust(6, "0"))
        if raw_time < last_time:
            raise EvidenceError(f"candump line {number}: timestamp regressed")
        if first_time is None:
            first_time = raw_time
        last_time = raw_time
        rtr = content.startswith("R")
        if rtr:
            data_hex = ""
            dlc = int(content[1:]) if content[1:] else 0
        else:
            if len(content) % 2 or len(content) > 16:
                raise EvidenceError(f"candump line {number}: invalid Classic CAN payload")
            data_hex = content.upper()
            dlc = len(data_hex) // 2
        frames.append({
            "t_us": raw_time - first_time,
            "can_id": f"{can_id:08X}",
            "data_hex": data_hex,
            "dlc": dlc,
            "extended": extended,
            "rtr": rtr,
        })
    if not frames:
        raise EvidenceError("candump capture is empty")
    rows = [{
        "type": "capture_meta",
        "schema": 1,
        "lost_frames": lost_frames,
        "interface": interface,
        "source_sha256": hashlib.sha256(source).hexdigest(),
        "timestamp_origin": "microseconds_since_first_candump_frame",
    }] + frames
    return ("\n".join(json.dumps(row, sort_keys=True, separators=(",", ":"))
                      for row in rows) + "\n").encode("utf-8")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("candump_source", type=Path)
    p.add_argument("output_jsonl", type=Path)
    p.add_argument("--interface", required=True,
                   help="exact can-utils interface name, e.g. can0")
    p.add_argument("--lost-frames", required=True, type=int,
                   help="measured loss counter from independent analyzer (never guess)")
    a = p.parse_args()
    try:
        if a.candump_source.resolve() == a.output_jsonl.resolve():
            raise EvidenceError("source and output paths must differ")
        source = a.candump_source.read_bytes()
        converted = convert(source, interface=a.interface,
                            lost_frames=a.lost_frames)
        # Refuse overwrite: evidence exports are immutable by convention.
        with a.output_jsonl.open("xb") as out:
            out.write(converted)
    except (OSError, EvidenceError, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    print("PASS OFFLINE CANDUMP CONVERSION (NOT HARDWARE APPROVAL): " +
          f"input_sha256={hashlib.sha256(source).hexdigest()} " +
          f"output_sha256={hashlib.sha256(converted).hexdigest()} " +
          f"frames={len(converted.splitlines()) - 1}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
