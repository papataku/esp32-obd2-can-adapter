#!/usr/bin/env python3
"""Read-only verification of proposed single-factory-app 8MiB M5CAN layout."""
import argparse
from pathlib import Path
from check_canlog_partition import read_layout, OLD_FIXED

FLASH_BYTES = 8 * 1024 * 1024
EXPECTED = {
    **OLD_FIXED,
    "factory": (0x10000, 0x200000),
    "canlog": (0x210000, 0x460000),
}

def validate(path: Path, firmware: Path | None = None) -> None:
    entries = read_layout(path)
    if set(entries) != set(EXPECTED):
        raise ValueError(f"partition names differ: {set(entries) ^ set(EXPECTED)}")
    for name, (start, size) in EXPECTED.items():
        kind, subtype, offset, amount = entries[name]
        if (offset, amount) != (start, size):
            raise ValueError(f"{name}: offset or size differs")
        if offset < 0 or size <= 0 or offset + size > FLASH_BYTES:
            raise ValueError(f"{name}: out of flash bounds")
        if offset % 4096 or amount % 4096:
            raise ValueError(f"{name}: sector alignment")
        if kind == "app" and (offset % 65536 or amount % 65536):
            raise ValueError("app alignment")
    if entries["factory"][:2] != ("app", "factory"):
        raise ValueError("single app must be factory, not ota_0")
    if entries["canlog"][:2] != ("data", "0x40"):
        raise ValueError("canlog must be unmounted custom data subtype")
    sections = sorted((v[2], v[2] + v[3], name) for name, v in entries.items())
    for (_, stop, name), (next_start, _, next_name) in zip(sections, sections[1:]):
        if next_start < stop:
            raise ValueError(f"overlap: {name}/{next_name}")
    if firmware:
        count = firmware.stat().st_size
        if count > EXPECTED["factory"][1]:
            raise ValueError(f"firmware {count} exceeds 2MiB")
        print(f"App binary {count:,} / {EXPECTED['factory'][1]:,} bytes")
    print("OK: single 2MiB factory image, CAN RAW 4.375MiB, retained evidence 1.5MiB")
    print("NOT SAFE TO FLASH as an update: existing OTA slots/otadata require migration")

if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("csv_path", type=Path)
    p.add_argument("--firmware-bin", type=Path)
    args = p.parse_args()
    validate(args.csv_path, args.firmware_bin)
