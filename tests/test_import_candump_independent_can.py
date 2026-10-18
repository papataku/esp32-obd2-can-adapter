#!/usr/bin/env python3
"""Synthetic-only candump importer tests; no physical CAN devices involved."""
import hashlib
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from import_candump_independent_can import convert
from reconcile_multi_ecu_can_capture import reconcile, parse_raw_capture, EvidenceError
from test_multi_ecu_bench_trace import hardware_trace


class OfflineCandumpImportTests(unittest.TestCase):
    def test_classic_extended_standard_and_remote(self):
        raw = (b"(1609770956.314100) can0 18DA02F1#0322201200000000\n"
               b"(1609770956.314120) can0 7E8#04622012AA000000\n"
               b"(1609770956.314140) can0 123#R8\n"
               b"(1609770956.314160) can0 18DAF102#\n")
        converted = convert(raw, interface="can0", lost_frames=0)
        frames = parse_raw_capture(converted)
        self.assertEqual([x["t_us"] for x in frames], [0, 20, 40, 60])
        self.assertEqual(frames[0]["can_id"], "18DA02F1")
        self.assertEqual(frames[1]["can_id"], "000007E8")
        self.assertFalse(frames[1]["extended"])
        self.assertTrue(frames[2]["rtr"])
        self.assertEqual(frames[2]["dlc"], 8)
        self.assertEqual(frames[2]["data_hex"], "")
        self.assertEqual(frames[3]["dlc"], 0)
        meta = json.loads(converted.splitlines()[0])
        self.assertEqual(meta["source_sha256"], hashlib.sha256(raw).hexdigest())

    def test_reconcile_real_bytes_imported_from_fake_candump(self):
        trace = hardware_trace()
        raw_source = []
        for ev in trace[1:]:
            if ev.get("kind") in ("can_tx", "can_rx"):
                timestamp = 1_609_770_956_000_000 + ev["t_us"]
                seconds, micros = divmod(timestamp, 1_000_000)
                can_id = ev["can_id"]
                raw_source.append(f"({seconds}.{micros:06}) can0 "
                                  f"{can_id}#{ev['data_hex']}")
        content = ("\n".join(raw_source) + "\n").encode()
        canonical = convert(content, interface="can0", lost_frames=0)
        trace[0]["capture_sha256"] = hashlib.sha256(canonical).hexdigest()
        # Timestamp origin in independent file is first CAN TX at event t=12.
        result = reconcile(trace, canonical, clock_offset_us=-12,
                           max_skew_us=0)
        self.assertEqual(result["matched_frames"], 8)
        self.assertEqual(result["diagnostic_tx"], 3)

    def test_refuse_malformed_or_truncated_sources(self):
        cases = [
            b"(1.000001) can0 123##11234\n",   # CAN FD is not Classic CAN
            b"(1.000001) can0 123#AAA\n",     # odd hex count
            b"(1.000001) can0 123#000000000000000000\n",  # > 8B
            b"(1.000001) can0 3FFFFFFF#00\n", # ID outside 29-bit
            b"(1.000001) can0 800#00\n",      # ID outside 11-bit
            b"can0 123#1122\n",               # lacks timestamp
            b"(1.000001) can0 123#GG\n",
        ]
        for item in cases:
            with self.subTest(item=item):
                with self.assertRaises(EvidenceError):
                    convert(item, interface="can0", lost_frames=0)

    def test_strict_interface_and_monotonic_timestamps(self):
        with self.assertRaisesRegex(EvidenceError, "interface changed"):
            convert(b"(1.000001) can0 123#AA\n"
                    b"(1.000002) can1 123#BB\n",
                    interface="can0", lost_frames=0)
        with self.assertRaisesRegex(EvidenceError, "timestamp regressed"):
            convert(b"(2.000002) can0 123#AA\n"
                    b"(1.999999) can0 123#BB\n",
                    interface="can0", lost_frames=0)
        with self.assertRaisesRegex(EvidenceError, "empty"):
            convert(b"", interface="can0", lost_frames=0)

    def test_loss_counter_mandatory_and_reconciler_rejects_loss(self):
        raw = b"(1.000001) can0 123#AA\n"
        with self.assertRaisesRegex(EvidenceError, "loss counter"):
            convert(raw, interface="can0", lost_frames=None)
        with self.assertRaisesRegex(EvidenceError, "loss counter"):
            convert(raw, interface="can0", lost_frames=-1)
        output = convert(raw, interface="can0", lost_frames=1)
        with self.assertRaisesRegex(EvidenceError, "frame-loss counter"):
            parse_raw_capture(output)

    def test_utf8_invalid_and_unsupported_interface(self):
        with self.assertRaisesRegex(EvidenceError, "UTF-8"):
            convert(b"\xff", interface="can0", lost_frames=0)
        with self.assertRaisesRegex(EvidenceError, "interface name"):
            convert(b"(1.000001) can0 123#AA\n",
                    interface="can 0", lost_frames=0)


if __name__ == "__main__":
    unittest.main()
