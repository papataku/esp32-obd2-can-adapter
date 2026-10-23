#!/usr/bin/env python3
"""Synthetic-only old failure / new success contrast regression cases."""
import hashlib
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from compare_ble574_v1_v2 import compare, parse_v1, require_digest  # noqa: E402


def old_v1(final=574, linked=0, status=0, newline=True, mtu=None):
    del mtu  # V1 has no field; tests must never fill one synthetically.
    s = (f"RING_EST_TRACE_V1 seq=1 path=UNKNOWN type=0 api_seen=1 "
         f"api_rc=0 api_dt_ms=0 gap_seen=1 gap_status={status} "
         f"gap_dt_ms=34 duplicates=0 final_rc={final} linked={linked} "
         f"overflow=0 END")
    return (s + ("\n" if newline else "")).encode()


def new_v2(seq=2, final=0, linked=1, mtu=247):
    return (f"RING_EST_TRACE_V2 seq={seq} path=SUCCESS api=0 gap=0 gap_dt=22 "
            f"disc=-1 disc_dt=-1 mtu_api=0 mtu_event={mtu} mtu_dt=98 "
            f"final={final} linked={linked} dup=0 ovf=0 END\n").encode()


def dual(clients=0, connects=0, disconnects=0, notify=0):
    return (f"#BLE_DUAL,clients={clients},connects={connects},"
            f"disconnects={disconnects},rx_overflow=0,notify={notify},"
            "retries=0,failed=0\n").encode()


def edge(con=0, ok=0, err=0, disc=0):
    return (f"#BLE_EDGE,con={con},ok={ok},err={err},last=0,last_ms=0,"
            f"first=-1,first_ms=0,disc={disc},reason=-1,disc_ms=0 END\n").encode()


def pair():
    return {
        "failure_ring": old_v1(),
        "failure_m5": dual(0, 11, 11, 1000) + dual(0, 11, 11, 1000),
        "success_ring": new_v2(),
        "success_m5": (edge() + dual() + edge(1, 1) +
                       dual(1, 1, 0, 244)),
    }


class LegacyV1ContrastTests(unittest.TestCase):
    def test_contrast_is_observational_not_causal(self):
        p = pair()
        result = compare(**p)
        self.assertEqual(result["classification"],
                         "V1_574_VS_V2_SUCCESS_CONTRAST_ONLY")
        self.assertEqual(result["legacy_failure"]["ring_v1_trace"]["final_rc"], 574)
        self.assertEqual(result["new_success"]["ring_v2_trace"]["seq"], 2)
        self.assertEqual(result["comparison"]["legacy_v1_mtu_event"],
                         "NOT_INSTRUMENTED")
        self.assertEqual(result["comparison"]["legacy_v1_early_disconnect"],
                         "NOT_INSTRUMENTED")
        self.assertEqual(result["error_decode_if_574"]["hci_error_hex"], "0x3e")
        self.assertEqual(result["error_decode_if_574"]["root_cause"],
                         "NOT_DETERMINED")
        self.assertIn("M5 firmware", " ".join(result["warnings"]))
        self.assertEqual(result["anomalies"], [])

    def test_v2_seq_is_not_user_connection_count(self):
        p = pair()
        p["success_ring"] = new_v2(seq=8)
        self.assertEqual(compare(**p)["classification"],
                         "V1_574_VS_V2_SUCCESS_CONTRAST_ONLY")

    def test_legacy_line_without_terminator_not_accepted(self):
        p = pair()
        p["failure_ring"] = old_v1(newline=False)
        result = compare(**p)
        self.assertEqual(result["classification"], "INCOMPLETE_EVIDENCE")
        self.assertEqual(result["legacy_failure"]["ring_v1_count"], 0)
        self.assertEqual(result["legacy_failure"]["ring_v1_invalid"], 1)

    def test_legacy_second_line_forces_stop(self):
        p = pair()
        p["failure_ring"] += old_v1()
        result = compare(**p)
        self.assertEqual(result["classification"], "STOP_ANOMALY")
        self.assertIn("multiple_complete_legacy_v1_records", result["anomalies"])

    def test_legacy_unknown_mtu_is_never_treated_as_failure(self):
        p = pair()
        self.assertEqual(compare(**p)["comparison"]["legacy_v1_mtu_api_return"],
                         "NOT_INSTRUMENTED")

    def test_old_gap_failure_would_not_match_known_574_evidence(self):
        p = pair()
        p["failure_ring"] = old_v1(status=62)
        self.assertEqual(compare(**p)["classification"],
                         "INCOMPLETE_OR_NONMATCHING_EVIDENCE")

    def test_new_success_requires_m5_gap_connect_confirmation(self):
        p = pair()
        p["success_m5"] = edge() + dual() + edge() + dual(0, 0, 0, 0)
        self.assertEqual(compare(**p)["classification"],
                         "INCOMPLETE_OR_NONMATCHING_EVIDENCE")

    def test_new_success_requires_m5_application_connection(self):
        p = pair()
        p["success_m5"] = edge() + dual() + edge(1, 1) + dual(0, 0, 0)
        self.assertEqual(compare(**p)["classification"],
                         "INCOMPLETE_OR_NONMATCHING_EVIDENCE")

    def test_peer_limit_anomaly_stop(self):
        p = pair()
        p["success_m5"] = edge() + dual(3, 3, 0) + edge(1, 1)
        self.assertEqual(compare(**p)["classification"], "STOP_ANOMALY")

    def test_old_m5_has_no_edge_instrumentation(self):
        p = pair()
        r = compare(**p)
        self.assertEqual(r["comparison"]["m5_gap_connect_failure_trial"],
                         "NOT_INSTRUMENTED")

    def test_hash_gate_rejects_substitution(self):
        data = b"private bytes"
        require_digest("failure_ring", data, hashlib.sha256(data).hexdigest())
        with self.assertRaisesRegex(ValueError, "SHA256 mismatch"):
            require_digest("failure_ring", data, "0" * 64)
        with self.assertRaisesRegex(ValueError, "invalid expected SHA256"):
            require_digest("failure_ring", data, "not-a-digest")

    def test_no_usb_payload_or_identity_leaks_into_output(self):
        p = pair()
        p["success_m5"] = b"\xffPRIVATE_ID_BLOB\n" + p["success_m5"]
        result = compare(**p)
        self.assertNotIn("PRIVATE_ID_BLOB", repr(result))

    def test_invalid_partial_old_row_fails_closed(self):
        p = pair()
        p["failure_ring"] += old_v1(newline=False)
        self.assertEqual(compare(**p)["classification"], "INCOMPLETE_EVIDENCE")


if __name__ == "__main__":
    unittest.main()
