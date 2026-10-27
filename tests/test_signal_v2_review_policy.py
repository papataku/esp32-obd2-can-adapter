#!/usr/bin/env python3
"""Tracked-asset policy: no physical BLE identity in reviewed lab sources."""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
class ReviewPolicy(unittest.TestCase):
    def test_no_physical_target_identifiers(self):
        for name in (
            "test_clients/smartring_signal_v2/client.h",
            "docs/qa/signal-v2-1-bench-runbook.md",
        ):
            source = (ROOT / name).read_text()
            self.assertIsNone(
                re.search(r"(?i)(?:[0-9a-f]{2}:){5}[0-9a-f]{2}", source),
                name,
            )
        target = "test_clients/smartring_signal_v2/signal_v2_target_local.h"
        self.assertIn(target, (ROOT / ".gitignore").read_text())
        self.assertIn("SIGNAL_V2_TARGET_ADDRESS",
                      (ROOT / (target + ".example")).read_text())
    def test_normal_diagnostics_are_redacted(self):
        gatt = (ROOT / "src/signal_v2_gatt.cpp").read_text()
        self.assertNotIn("ble_address=%s", gatt)
        self.assertIn("#SIGNALV2_ADMIN_PRIVATE,", gatt)
        self.assertIn("M5CAN_CAN_TX_HARD_DISABLED", gatt)
    def test_heartbeat_cannot_mask_missing_data(self):
        ring = (ROOT / "test_clients/smartring_signal_v2/client.h").read_text()
        self.assertIn("acceptsSequential(seq, frame, resync)", ring)
        self.assertLess(ring.index("if (b[4] != 0x21 && !acceptsSequential("),
                        ring.index("} else if (b[4] == 0x22)"))

if __name__ == "__main__":
    unittest.main()
