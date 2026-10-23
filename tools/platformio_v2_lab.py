# PlatformIO pre-build hook for a separately authorized M5CAN V2 LAB image.
# The value comes only from the operator environment, never the repository.
Import("env")
import os
import sys

sys.path.insert(0, os.path.join(env["PROJECT_DIR"], "tools"))
from v2_lab_identity import parse_allowed_peer_id48

value = os.environ.get("M5CAN_LOG_V2_ALLOWED_PEER_ID48", "")
try:
    approved_identity = parse_allowed_peer_id48(value)
except ValueError as exc:
    raise RuntimeError(
        "V2 Lab build BLOCKED: set M5CAN_LOG_V2_ALLOWED_PEER_ID48 to the "
        "explicitly approved BLE bonded identity (not a random detected peer). "
        + str(exc)
    ) from exc

# The NimBLE GATT callback re-checks the resolved identity, encryption and
# bond on every accepted control message. This CPPDEFINE is NOT a secret key.
env.Append(CPPDEFINES=[
    ("M5CAN_LOG_V2_ALLOWED_PEER_ID48", "0x%012XULL" % approved_identity),
])
print("M5CAN V2: explicit single-peer identity provisioned (value redacted)")
