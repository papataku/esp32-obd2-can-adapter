#!/usr/bin/env python3
"""The V2 lab firmware must not silently enroll an arbitrary BLE peer."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from v2_lab_identity import parse_allowed_peer_id48

for candidate in ("", "0x0", "0xFFFFFFFFFFFF", "0x000000000000",
                  "AA:BB:CC", "not-an-id", "0x1000000000000",
                  "0x00000000000000", "0x123456789ABG"):
    try:
        parse_allowed_peer_id48(candidate)
    except ValueError:
        pass
    else:
        raise AssertionError("Unsafe V2 identity accepted: " + candidate)

assert parse_allowed_peer_id48("0x112233445566") == 0x112233445566
assert parse_allowed_peer_id48("11:22:33:44:55:66") == 0x112233445566
assert parse_allowed_peer_id48("  0xA1b2C3d4E5f6  ") == 0xA1B2C3D4E5F6
print("PASS: authenticated V2 lab requires explicit valid 48-bit bonded identity")
