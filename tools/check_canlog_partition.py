#!/usr/bin/env python3
"""Read-only validation of a proposed 8MiB flash layout; never flash/erase."""
import argparse
import csv
from pathlib import Path

MIB = 1024 * 1024
FLASH = 8 * MIB
OLD_FIXED = {
    "nvs": (0x9000, 0x5000),
    "otadata": (0xE000, 0x2000),
    "spiffs": (0x670000, 0x180000),
    "coredump": (0x7F0000, 0x10000),
}
NEW_EXPECTED = {
    "app0": (0x10000, 0x200000),
    "app1": (0x210000, 0x200000),
    "canlog": (0x410000, 0x260000),
}

def read_layout(path):
    entries = {}
    with open(path, encoding="utf-8", newline="") as source:
        for row in csv.reader(line for line in source if not line.lstrip().startswith("#")):
            if not row or not row[0].strip():
                continue
            if len(row) < 5:
                raise ValueError("partition row missing columns")
            name, kind, subtype, offset, size = [part.strip() for part in row[:5]]
            if name in entries:
                raise ValueError("duplicate partition: " + name)
            entries[name] = (kind, subtype, int(offset, 0), int(size, 0))
    return entries

def validate(path, firmware=None):
    entries = read_layout(path)
    required = set(OLD_FIXED) | set(NEW_EXPECTED)
    if set(entries) != required:
        raise ValueError("unexpected partition labels; expected: " + ", ".join(sorted(required)))
    for name, expected in {**OLD_FIXED, **NEW_EXPECTED}.items():
        _, _, offset, size = entries[name]
        if (offset, size) != expected:
            raise ValueError(f"{name}: incorrect offset/size")
    for name, (kind, subtype, offset, size) in entries.items():
        if not size or offset < 0 or offset + size > FLASH:
            raise ValueError(f"{name}: outside 8MiB flash")
        if offset % 0x1000 or size % 0x1000:
            raise ValueError(f"{name}: not sector-aligned")
        if kind == "app" and (offset % 0x10000 or size % 0x10000):
            raise ValueError(f"{name}: app not 64KiB-aligned")
    intervals = sorted((x[2], x[2] + x[3], name) for name, x in entries.items())
    for (_, previous_end, previous_name), (next_start, _, next_name) in zip(intervals, intervals[1:]):
        if previous_end > next_start:
            raise ValueError(f"overlap: {previous_name}/{next_name}")
    if entries["canlog"][:2] != ("data", "0x40"):
        raise ValueError("canlog must be custom data subtype 0x40, not a mounted FS")
    if entries["app0"][:2] != ("app", "ota_0") or entries["app1"][:2] != ("app", "ota_1"):
        raise ValueError("must retain two OTA slots")
    if firmware is not None:
        actual_size = firmware.stat().st_size
        if actual_size > entries["app0"][3]:
            raise ValueError(f"firmware {actual_size} bytes exceeds 2MiB app slots")
        print(f"App binary {actual_size:,} bytes / {entries['app0'][3]:,} slot")
    print("Proposed layout verified: two 2MiB OTA slots; 2.375MiB raw canlog;"
          " old 1.5MiB evidence spiffs offset preserved.")
    print("NOT APPROVED FOR FLASHING: existing OTA slot data and new canlog"
          " region must be backed up and explicitly migrated.")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv_path", type=Path)
    parser.add_argument("--firmware-bin", type=Path)
    args = parser.parse_args()
    validate(args.csv_path, args.firmware_bin)
