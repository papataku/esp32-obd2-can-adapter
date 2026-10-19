"""Build-time safeguards for an explicitly erased 8 MiB M5Dial.

No erase, partition write, file deletion or device probing is performed here.
The operator's consent to data loss is explicitly scoped to this one lab env.
"""
Import("env")
import os
import sys
from pathlib import Path

root = Path(env["PROJECT_DIR"])
sys.path.insert(0, str(root / "tools"))
from check_single_app_canlog_partition import validate

expected = "ERASE_ALL_8MB_OLD_DATA_REQUIRED"
if env["PIOENV"] != "m5dial_v2_fresh_flash_lab":
    raise RuntimeError("V2 fresh-install gate was applied outside the intended lab profile")
if os.environ.get("M5CAN_V2_FRESH_FLASH_ACK", "") != expected:
    raise RuntimeError(
        "V2 fresh flash build BLOCKED: existing NVS/OTA/app/SD evidence data "
        "will be lost after a separate manual erase. Supply "
        "M5CAN_V2_FRESH_FLASH_ACK=" + expected + " to opt in. "
        "This build script does NOT erase, upload or change the device."
    )
layout = root / "partitions" / "m5can_8mb_single_app_canlog_proposed.csv"
validate(layout)
partition_file = Path(env.GetProjectOption("board_build.partitions"))
if partition_file.as_posix() != "partitions/m5can_8mb_single_app_canlog_proposed.csv":
    raise RuntimeError("V2 fresh flash: unexpected partition layout")
print("V2 lab: 8 MiB single-app/4.375 MiB CANLOG layout validated; no erase or upload")
