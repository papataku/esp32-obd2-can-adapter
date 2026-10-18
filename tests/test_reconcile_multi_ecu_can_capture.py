#!/usr/bin/env python3
"""Entire suite uses fabricated CAN bytes; no hardware evidence or CAN TX."""
import hashlib
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from reconcile_multi_ecu_can_capture import (EvidenceError, reconcile,
                                             parse_raw_capture)
from test_multi_ecu_bench_trace import hardware_trace, passive


def encode_raw(rows):
    return ("\n".join(json.dumps(r, sort_keys=True) for r in rows) +
            "\n").encode("utf-8")


def paired(*, offset=0, passive_profile=False):
    event_trace = passive() if passive_profile else hardware_trace()
    event_trace[0]["evidence"] = "hardware"
    event_trace[0]["observer"] = "SYNTHETIC QA FIXTURE - NOT HARDWARE"
    event_trace[0]["interface"] = "fake independent CAN analyzer"
    rows = [{"type": "capture_meta", "schema": 1, "lost_frames": 0}]
    for ev in event_trace[1:]:
        if ev.get("kind") in {"can_tx", "can_rx"}:
            if passive_profile:
                ev.update(data_hex="04622012AA000000", dlc=8,
                          extended=True, rtr=False)
            rows.append({key: ev[key] for key in
                         ("t_us", "can_id", "data_hex", "dlc", "extended", "rtr")})
            rows[-1]["t_us"] += offset
    content = encode_raw(rows)
    event_trace[0]["capture_sha256"] = hashlib.sha256(content).hexdigest()
    return event_trace, rows, content


def with_hash(trace, rows):
    data = encode_raw(rows)
    trace[0]["capture_sha256"] = hashlib.sha256(data).hexdigest()
    return data


class IndependentCaptureTests(unittest.TestCase):
    def must_fail(self, trace, raw, message, **kwargs):
        with self.assertRaisesRegex(EvidenceError, message):
            reconcile(trace, raw, **kwargs)

    def test_two_ecu_payloads_have_provenance_and_order(self):
        trace, rows, raw = paired()
        result = reconcile(trace, raw, max_skew_us=0)
        self.assertEqual(result["diagnostic_tx"], 3)
        self.assertEqual(result["matched_frames"], 8)
        self.assertEqual(result["unmatched_background_frames"], 0)
        self.assertEqual(result["sha256"], hashlib.sha256(raw).hexdigest())

    def test_tampered_raw_hash_refused(self):
        trace, rows, raw = paired()
        self.must_fail(trace, raw + b" ", "SHA-256 differs")

    def test_missing_first_frame_and_missing_flow_control_refused(self):
        for index in [3, 4]:
            trace, rows, _ = paired()
            raw = with_hash(trace, rows[:index] + rows[index+1:])
            self.must_fail(trace, raw, "missing/out-of-order frame")

    def test_changing_payload_even_with_updated_hash_refused(self):
        trace, rows, _ = paired()
        rows[5]["data_hex"] = "22DEADBEEF000000"
        raw = with_hash(trace, rows)
        self.must_fail(trace, raw, "missing/out-of-order frame")

    def test_extra_diagnostic_tx_not_hidden_by_event_trace(self):
        trace, rows, _ = paired()
        rows.append({"t_us": 35, "can_id": "18DA03F1",
                     "data_hex": "0322201200000000", "dlc": 8,
                     "extended": True, "rtr": False})
        raw = with_hash(trace, rows)
        self.must_fail(trace, raw, "unaccounted physical diagnostic TX")

    def test_background_noise_permitted_only_for_non_tester_id(self):
        trace, rows, _ = paired()
        rows.append({"t_us": 35, "can_id": "18DAF180",
                     "data_hex": "04622012AA000000", "dlc": 8,
                     "extended": True, "rtr": False})
        raw = with_hash(trace, rows)
        result = reconcile(trace, raw, max_skew_us=0)
        self.assertEqual(result["unmatched_background_frames"], 1)

    def test_tx_off_needs_independent_raw_and_denies_any_tester_frame(self):
        trace, rows, raw = paired(passive_profile=True)
        result = reconcile(trace, raw)
        self.assertEqual(result["diagnostic_tx"], 0)
        rows.append({"t_us": 10, "can_id": "18DA02F1",
                     "data_hex": "0322201200000000", "dlc": 8,
                     "extended": True, "rtr": False})
        raw = with_hash(trace, rows)
        self.must_fail(trace, raw, "TX=OFF profile has physical diagnostic")

    def test_functional_tester_id_not_ignored(self):
        trace, rows, _ = paired()
        rows.append({"t_us": 31, "can_id": "18DB33F1",
                     "data_hex": "02010C0000000000", "dlc": 8,
                     "extended": True, "rtr": False})
        raw = with_hash(trace, rows)
        self.must_fail(trace, raw, "unaccounted physical diagnostic TX")

    def test_observed_clock_offset_must_be_explicit(self):
        trace, rows, raw = paired(offset=9_000)
        self.must_fail(trace, raw, "missing/out-of-order frame",
                       max_skew_us=50)
        self.assertEqual(
            reconcile(trace, raw, clock_offset_us=9_000,
                      max_skew_us=50)["matched_frames"], 8)

    def test_analyzer_reports_zero_packet_loss(self):
        trace, rows, _ = paired()
        rows[0]["lost_frames"] = 1
        raw = with_hash(trace, rows)
        self.must_fail(trace, raw, "frame-loss counter")

    def test_invalid_capture_flags_and_data(self):
        for key, value, pattern in [
            ("extended", False, "expected extended"),
            ("rtr", True, "expected extended"),
            ("dlc", 7, "DLC mismatch"),
            ("data_hex", "XYZ", "invalid Classical CAN"),
            ("t_us", -1, "invalid/regressed timestamp"),
        ]:
            trace, rows, _ = paired()
            rows[1][key] = value
            raw = with_hash(trace, rows)
            self.must_fail(trace, raw, pattern)

    def test_regressed_can_analyzer_timestamp(self):
        trace, rows, _ = paired()
        rows[2]["t_us"] = 0
        raw = with_hash(trace, rows)
        self.must_fail(trace, raw, "invalid/regressed timestamp")

    def test_missing_raw_frame_metadata_fails(self):
        trace, rows, _ = paired()
        rows = rows[1:]
        raw = with_hash(trace, rows)
        self.must_fail(trace, raw, "capture metadata missing")

    def test_require_hardware_schema_not_synthetic(self):
        trace, rows, raw = paired()
        trace[0]["evidence"] = "synthetic"
        self.must_fail(trace, raw, "requires hardware-schema")

    def test_invalid_maximum_skew_rejected(self):
        trace, rows, raw = paired()
        self.must_fail(trace, raw, "invalid clock offset", max_skew_us=-1)


if __name__ == "__main__":
    unittest.main()
