"""Parse an explicitly approved BLE identity for opt-in M5CAN V2 GATT.

No default identity, no auto-bond/auto-enroll, and no material stored in
source control. This is a build-time opt-in, NOT proof of real MITM security.
"""
import re


def parse_allowed_peer_id48(text: str) -> int:
    value = text.strip()
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", value):
        value = "0x" + value.replace(":", "")
    if not re.fullmatch(r"0[xX][0-9A-Fa-f]{12}", value):
        raise ValueError("Expected 0x followed by 12 hexadecimal digits, or AA:BB:CC:DD:EE:FF")
    numeric = int(value, 16)
    if numeric in (0, (1 << 48) - 1):
        raise ValueError("The all-zero and broadcast identities are not allowed")
    return numeric
