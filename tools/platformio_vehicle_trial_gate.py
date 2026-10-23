"""PlatformIO prebuild gate for ALREADY-MIGRATED app-only trial firmware.

NEVER erase/upload here. The installed-device layout must be separately
read back and verified immediately before any app-only physical flash.
"""
Import("env")
import os
import sys
from pathlib import Path

root = Path(env["PROJECT_DIR"])
sys.path.insert(0, str(root / "tools"))
from check_single_app_canlog_partition import validate
from vehicle_trial_profile import validate_vehicle_trial_build

try:
    validate_vehicle_trial_build(
        env["PIOENV"],
        str(env.GetProjectOption("board_build.partitions")),
        os.environ.get("M5CAN_VEHICLE_TRIAL_APPROVED_PEER_ID48", ""),
        os.environ.get("M5CAN_VEHICLE_TRIAL_INSTALLED_PARTITIONS_SHA256", ""),
        os.environ.get("M5CAN_VEHICLE_TRIAL_APP_ONLY_ACK", ""),
    )
    if os.environ.get("M5CAN_V2_FRESH_FLASH_ACK"):
        raise ValueError("stale destructive fresh-flash acknowledgement is forbidden")
    validate(root / "partitions" / "m5can_8mb_single_app_canlog_proposed.csv")
except ValueError as exc:
    raise RuntimeError("M5CAN vehicle-trial build BLOCKED: " + str(exc)) from exc

print("M5CAN vehicle trial: existing 8MiB single-app partition pinned")
print("M5CAN vehicle trial: private identity validated but NOT a V1 BLE runtime allowlist")
print("M5CAN vehicle trial: NO flash, erase, partition write or NVS migration permitted")
