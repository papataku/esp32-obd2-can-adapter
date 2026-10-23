#!/usr/bin/env python3
"""Offline tests for non-destructive, opt-in dual-BLE V1 vehicle trial."""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from vehicle_trial_profile import (
    APP_ONLY_ACK, ARCHIVED_INSTALLED_PARTITIONS_SHA256,
    LAYOUT, PROFILE, validate_vehicle_trial_build,
)
from check_single_app_canlog_partition import validate


def approved():
    return dict(profile=PROFILE, partition_path=LAYOUT,
                peer_id="0x010203040506",
                installed_partition_sha256=ARCHIVED_INSTALLED_PARTITIONS_SHA256,
                acknowledgement=APP_ONLY_ACK)


class ProfileTests(unittest.TestCase):
    def test_passes_only_explicit_opt_in(self):
        self.assertEqual(validate_vehicle_trial_build(**approved()), PROFILE)

    def test_reject_wrong_env(self):
        args = approved()
        args["profile"] = "m5dial"
        with self.assertRaisesRegex(ValueError, "outside exact"):
            validate_vehicle_trial_build(**args)

    def test_reject_old_ota_layout(self):
        args = approved()
        args["partition_path"] = "partitions/default.csv"
        with self.assertRaisesRegex(ValueError, "exact existing"):
            validate_vehicle_trial_build(**args)

    def test_reject_missing_and_wrong_approval(self):
        for value in ("", "ERASE_ALL_8MB_OLD_DATA_REQUIRED", "APP_ONLY"):
            args = approved()
            args["acknowledgement"] = value
            with self.subTest(value=value), self.assertRaisesRegex(
                    ValueError, "APP-ONLY"):
                validate_vehicle_trial_build(**args)

    def test_requires_matching_installed_partition_hash(self):
        for value in ("", "0"*64, "Z"*64):
            args = approved()
            args["installed_partition_sha256"] = value
            with self.subTest(value=value), self.assertRaisesRegex(
                    ValueError, "partition"):
                validate_vehicle_trial_build(**args)

    def test_reject_missing_fake_or_broadcast_identity(self):
        for value in ("", "00:00:00:00:00:00",
                      "FF:FF:FF:FF:FF:FF", "INVALID"):
            args = approved()
            args["peer_id"] = value
            with self.subTest(value=value), self.assertRaisesRegex(
                    ValueError, "identity"):
                validate_vehicle_trial_build(**args)

    def test_identity_never_in_validation_return(self):
        args = approved()
        args["peer_id"] = "AA:BB:CC:DD:EE:01"
        self.assertEqual(validate_vehicle_trial_build(**args), PROFILE)

    def test_csv_layout_statically_valid(self):
        validate(ROOT / LAYOUT)

    def test_trial_is_strictly_v1_without_demo_auto(self):
        ini = (ROOT / "platformio.ini").read_text()
        section = ini.split("[env:" + PROFILE + "]", 1)[1].split("[env:", 1)[0]
        self.assertIn("env:m5dial_dual_ble_multipid_mtu_bench", section)
        self.assertIn("board_build.partitions = " + LAYOUT, section)
        self.assertIn("platformio_vehicle_trial_gate.py", section)
        self.assertNotIn("M5CAN_LOG_STREAM_V2_EXPERIMENTAL", section)
        self.assertNotIn("M5CAN_CAN_MODE_DRY_RUN", section)
        self.assertIn("M5CAN_VEHICLE_TRIAL=1", section)
        hdr = (ROOT / "src/serial_protocol.h").read_text()
        source = (ROOT / "src/serial_protocol.cpp").read_text()
        self.assertIn("std::atomic<DemoSetting> demo_setting_{DemoSetting::Off};",
                      hdr)
        self.assertIn("#if !defined(M5CAN_VEHICLE_TRIAL)", source)
        self.assertIn("VEHICLE_TRIAL_DEMO_LOCKED_OFF", source)
        main = (ROOT / "src/main.cpp").read_text()
        self.assertIn("Vehicle-trial profile must not contain V2 bulk", main)


if __name__ == "__main__":
    unittest.main()
