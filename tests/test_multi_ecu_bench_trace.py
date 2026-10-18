#!/usr/bin/env python3
"""Synthetic regression cases for offline bench evidence verification.

All events here are invented deterministic fixtures, NOT actual CAN captures.
"""
import copy
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from validate_multi_ecu_bench_trace import EvidenceError, verify  # noqa: E402

SHA = "a" * 40


def meta(profile="isolated_can_bench", evidence="synthetic"):
    return {"type": "meta", "schema": 1, "profile": profile,
            "evidence": evidence, "firmware_sha": SHA}


def event(t, kind, **kw):
    return {"type": "event", "t_us": t, "kind": kind, **kw}


def standard():
    # Complete synthetic two-ECU trace with two peers, a foreign responder,
    # same-DID joined peer, then BLE handle reuse in a new generation.
    return [
        meta(),
        event(1, "ble_connect", conn=7, generation=1),
        event(2, "ble_connect", conn=19, generation=1),
        event(10, "request_begin", id="r02", conn=7, generation=1,
              can_id="18DA02F1", service="22", did="2012"),
        event(11, "ble_join", conn=19, generation=1,
              can_id="18DA02F1", service="22", did="2012"),
        event(12, "can_tx", id="r02", frame="request", can_id="18DA02F1"),
        event(13, "can_rx", can_id="18DAF106", pci="ff",
              service="62", did="2012"),  # foreign FF, never FC
        event(14, "can_rx", can_id="18DAF102", pci="ff",
              service="62", did="2012"),
        event(15, "can_tx", id="r02", frame="fc", can_id="18DA02F1"),
        event(16, "can_rx", can_id="18DAF106", pci="cf",
              seq=1, complete=True),
        event(17, "can_rx", can_id="18DAF102", pci="cf",
              seq=1, complete=True),
        event(18, "request_finish", id="r02", status="ok"),
        event(19, "ble_deliver", id="r02", conn=7,
              generation=1, status="ok"),
        event(20, "ble_deliver", id="r02", conn=19,
              generation=1, status="ok"),
        event(21, "ble_disconnect", conn=7, generation=1),
        event(22, "ble_connect", conn=7, generation=2),
        event(23, "request_begin", id="r06", conn=7, generation=2,
              can_id="18DA06F1", service="22", did="2012"),
        event(24, "can_tx", id="r06", frame="request", can_id="18DA06F1"),
        event(25, "can_rx", can_id="18DAF106", pci="sf",
              service="62", did="2012"),
        event(26, "request_finish", id="r06", status="ok"),
        event(27, "ble_deliver", id="r06", conn=7,
              generation=2, status="ok"),
    ]


def passive():
    return [
        meta(profile="passive_tx_off"),
        event(1, "ble_connect", conn=7, generation=1),
        event(2, "request_denied", reason="tx_off"),
        event(3, "can_rx", can_id="18DAF102", pci="sf",
              service="62", did="2012"),
        event(4, "ble_disconnect", conn=7, generation=1)
    ]


class BenchEvidenceValidationTests(unittest.TestCase):
    def fails(self, records, reason):
        with self.assertRaisesRegex(EvidenceError, reason):
            verify(records)

    def test_two_ecu_join_and_connection_reuse_pass(self):
        result = verify(standard())
        self.assertEqual(result["requests"], 2)
        self.assertEqual(result["fc"], 1)
        self.assertEqual(result["foreign_rx"], 2)
        self.assertEqual(result["delivered"], 3)
        self.assertEqual(result["evidence"], "synthetic")

    def test_tx_disabled_profile_passes_without_can_tx(self):
        result = verify(passive())
        self.assertEqual(result["can_tx"], 0)
        self.assertEqual(result["denied"], 1)

    def test_tx_off_fails_for_any_real_request_or_fc(self):
        data = passive()
        data.insert(2, event(1, "can_tx", id="bad",
                             frame="fc", can_id="18DA02F1"))
        self.fails(data, "CAN TX or Flow Control seen in TX=OFF")

    def test_fc_wrong_ecu(self):
        data = standard()
        data[8]["can_id"] = "18DA06F1"
        self.fails(data, "CAN TX to wrong ECU")

    def test_fc_without_first_frame(self):
        data = standard()
        del data[7]  # delete the matching FF; remaining foreign FF is noise
        self.fails(data, "Flow Control without matching first frame")

    def test_foreign_ecu_cannot_satisfy_success(self):
        data = standard()
        del data[7]  # matching FF
        del data[7]  # FC
        del data[8]  # matching CF; foreign frames alone cannot complete
        self.fails(data, "successful request without matched ISO-TP data")

    def test_uds_rejects_did_mismatch(self):
        data = standard()
        data[7]["did"] = "2013"
        self.fails(data, "mismatched UDS positive response")

    def test_must_be_read_only_and_allowlisted(self):
        for key, value in [("service", "2E"), ("did", "ZZZZ"),
                           ("can_id", "18DA03F1")]:
            data = standard()
            data[3][key] = value
            self.fails(data, "only exact read-only|unapproved physical")

    def test_two_in_flight_queries_forbidden(self):
        data = standard()
        data.insert(6, event(12, "request_begin", id="extra",
                             conn=19, generation=1,
                             can_id="18DA06F1", service="22", did="2012"))
        self.fails(data, "overlapping physical CAN requests")

    def test_cross_did_join_forbidden(self):
        data = standard()
        data[4]["did"] = "2013"
        self.fails(data, "joined peer request identity differs")

    def test_disconnected_peer_cannot_receive(self):
        data = standard()
        data.insert(11, event(17, "ble_disconnect", conn=19, generation=1))
        self.fails(data, "BLE delivery to disconnected or reused generation")

    def test_reused_handle_old_generation_rejected(self):
        data = standard()
        data[20]["generation"] = 1
        self.fails(data, "BLE delivery to disconnected or reused generation")

    def test_peer_not_subscribed_cannot_receive(self):
        data = standard()
        del data[4]  # remove valid joined-peer subscription
        self.fails(data, "peer not subscribed")

    def test_corrupted_cf_sequence(self):
        data = standard()
        data[10]["seq"] = 2
        self.fails(data, "CF sequence gap")

    def test_no_ble_delivery_before_completion(self):
        data = standard()
        data[12], data[11] = data[11], data[12]
        # Equal timestamp to retain monotonicity while testing state.
        data[11]["t_us"] = 18
        data[12]["t_us"] = 18
        self.fails(data, "BLE delivery before CAN transaction finish")

    def test_timestamp_order_and_end_of_trace(self):
        data = standard()
        data[7]["t_us"] = 5
        self.fails(data, "timestamps are not monotonic")
        data = standard()[:9]
        self.fails(data, "trace ends with active CAN request")

    def test_fault_lock_blocks_further_can_tx(self):
        data = standard()
        data.insert(17, event(23, "fault_lock"))
        self.fails(data, "CAN TX after fault lock")

    def test_hardware_provenance_is_required_not_assumed(self):
        data = standard()
        data[0]["evidence"] = "hardware"
        self.fails(data, "capture_sha256")
        data[0]["capture_sha256"] = "b" * 64
        self.fails(data, "observer and interface")
        data[0]["observer"] = "Independent test operator"
        data[0]["interface"] = "isolated test CAN analyzer"
        self.assertEqual(verify(data)["evidence"], "hardware")

    def test_invalid_or_unknown_schema_never_falls_back(self):
        for bad_key, bad_value in [("schema", 2), ("profile", "vehicle"),
                                   ("evidence", "trust_me")]:
            data = standard()
            data[0][bad_key] = bad_value
            self.fails(data, "unsupported schema|invalid profile|invalid evidence")

    def test_passive_without_denied_operation_is_not_a_pass(self):
        data = passive()
        data = [e for e in data if e.get("kind") != "request_denied"]
        self.fails(data, "passive trace lacks TX=OFF denial")


if __name__ == "__main__":
    unittest.main()
