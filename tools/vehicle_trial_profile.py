"""Pure build-only checks for an ALREADY-MIGRATED M5Dial trial image.

The peer identity validates private build provenance; V1 ELM does NOT provide
a runtime BLE peer allowlist. This helper never prints/stores that identity.
No device access, erase, flash, formatting or migration is performed.
"""
import re

from v2_lab_identity import parse_allowed_peer_id48

PROFILE = "m5dial_existing_singleapp_vehicle_trial"
LAYOUT = "partitions/m5can_8mb_single_app_canlog_proposed.csv"
ARCHIVED_INSTALLED_PARTITIONS_SHA256 = (
    "7891dd9f69f2da4a49b4ed63ec4bd1651f9258d0f0787249e3f7bae65591645f"
)
APP_ONLY_ACK = "APP_ONLY_NO_ERASE_KEEP_NONAPP"


def validate_vehicle_trial_build(profile, partition_path, peer_id,
                                 installed_partition_sha256, acknowledgement):
    if profile != PROFILE:
        raise ValueError("vehicle trial gate used outside exact opt-in env")
    if partition_path != LAYOUT:
        raise ValueError("vehicle trial requires exact existing single-app layout")
    if acknowledgement != APP_ONLY_ACK:
        raise ValueError("vehicle trial APP-ONLY acknowledgement missing; NO ERASE")
    if not re.fullmatch(r"[a-fA-F0-9]{64}", installed_partition_sha256):
        raise ValueError("vehicle trial installed partition digest missing/invalid")
    if installed_partition_sha256.lower() != ARCHIVED_INSTALLED_PARTITIONS_SHA256:
        raise ValueError("vehicle trial partition SHA differs from archived install")
    try:
        parse_allowed_peer_id48(peer_id)
    except ValueError as exc:
        raise ValueError("vehicle trial approved BLE identity missing/invalid") from exc
    # Identity is intentionally not emitted or compiled into V1 firmware.
    return PROFILE
