#!/usr/bin/env python3
"""No hardware access: exercise complete-line BLE evidence audit."""
import hashlib
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from analyze_ble_linkedge_single import analyze  # noqa: E402


def trace(final=0, path="SUCCESS", linked=1, seq=1, newline=True):
    s = (f"RING_EST_TRACE_V2 seq={seq} path={path} api=0 gap=0 gap_dt=28 "
         f"disc=-1 disc_dt=-1 mtu_api=0 mtu_event=247 mtu_dt=86 "
         f"final={final} linked={linked} dup=0 ovf=0 END")
    return (s + ("\n" if newline else "")).encode()


def edge(con=0, ok=0, err=0, disc=0):
    return (f"#BLE_EDGE,con={con},ok={ok},err={err},last=0,last_ms=0,"
            f"first=-1,first_ms=0,disc={disc},reason=-1,disc_ms=0 END\n").encode()


def dual(clients=0, connects=0, disconnects=0, notify=0):
    return (f"#BLE_DUAL,clients={clients},connects={connects},"
            f"disconnects={disconnects},rx_overflow=0,notify={notify},"
            "retries=0,failed=0\n").encode()


class SingleShotAuditTests(unittest.TestCase):
    def test_ring_and_m5_complete_success(self):
        ring = trace()
        m5 = (edge() + dual() + b"#BLE_EDGE_TASK,interpoll_peak_us=1730 END\n"
              + edge(1, 1) + dual(1, 1, 0, 243))
        r = analyze(ring, m5)
        self.assertEqual(r["classification"], "RING_SINGLE_CONNECT_SUCCESS_ONLY")
        self.assertEqual(r["ring"]["complete_v2"][0]["mtu_event"], 247)
        self.assertEqual(r["m5"]["gap"]["last"]["ok"], 1)
        self.assertEqual(r["m5"]["dual"]["last"]["clients"], 1)
        self.assertEqual(r["m5"]["interpoll_peak_us"], 1730)
        self.assertEqual(r["m5"]["sha256"], hashlib.sha256(m5).hexdigest())
        self.assertEqual(r["anomalies"], [])

    def test_old_574_is_not_reclassified_as_success(self):
        r = analyze(trace(final=574, path="DISCONNECT", linked=0),
                    edge() + dual() + edge(0, 0, 0, 0))
        self.assertEqual(r["classification"], "FAIL_STOP")
        self.assertEqual(r["ring"]["complete_v2"][0]["final"], 574)

    def test_truncated_usb_line_never_proves_success(self):
        r = analyze(trace(newline=False), edge() + dual())
        self.assertEqual(r["classification"], "INCOMPLETE_EVIDENCE")
        self.assertEqual(r["ring"]["incomplete_v2"], 1)

    def test_missing_end_is_never_a_valid_trace(self):
        r = analyze(trace().replace(b" END\n", b"\n"), edge() + dual())
        self.assertEqual(r["classification"], "INCOMPLETE_EVIDENCE")
        self.assertEqual(r["ring"]["incomplete_v2"], 1)

    def test_duplicate_attempt_trips_stop(self):
        r = analyze(trace(seq=1) + trace(seq=2), edge() + dual())
        self.assertEqual(r["classification"], "STOP_ANOMALY")
        self.assertIn("more_than_one_ring_trace_in_capture", r["anomalies"])

    def test_more_than_two_clients_trips_stop(self):
        r = analyze(trace(), edge() + dual(3, 3, 0))
        self.assertEqual(r["classification"], "STOP_ANOMALY")
        self.assertIn("m5_peer_limit_exceeded", r["anomalies"])

    def test_counter_reset_during_capture_is_anomaly(self):
        r = analyze(trace(), edge(1, 1) + edge(0, 0) +
                    dual(1, 10, 4, 40) + dual(0, 0, 0, 0))
        self.assertEqual(r["classification"], "STOP_ANOMALY")
        self.assertIn("m5_edge_counter_regression", r["anomalies"])
        self.assertIn("m5_dual_counter_regression", r["anomalies"])

    def test_private_binary_lines_do_not_leak_payload(self):
        m5 = b"\x00\xffPRIVATE_PEER_ID\n" + edge() + dual()
        r = analyze(trace(), m5)
        self.assertNotIn("PRIVATE_PEER_ID", repr(r))
        self.assertEqual(r["m5"]["dual"]["samples"], 1)

    def test_malformed_unterminated_edge_does_not_count(self):
        m5 = edge().rstrip(b"\n") + dual()
        r = analyze(trace(), m5)
        self.assertIsNone(r["m5"]["gap"])
        self.assertGreater(r["m5"]["incomplete"]["edge"], 0)

    def test_no_cross_device_causality_claim(self):
        r = analyze(trace(), edge() + dual())
        self.assertTrue(any("timestamp" in s for s in r["warnings"]))


if __name__ == "__main__":
    unittest.main()
