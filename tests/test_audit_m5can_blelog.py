#!/usr/bin/env python3
"""Synthetic BLELOG v3/v2 fixtures; NOT hardware BLE session validation."""
import hashlib
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from audit_m5can_blelog import audit_blelog, EvidenceError

BOOT = ("#BLELOG,BOOT,boot=24,state=persisted,version=3,"
        "build=0123456789abcdef,total=2,kept=2,evicted=0,"
        "queue_drops=0,seq_gaps=0,first_seq=1,last_seq=2,saves=1")
INTEGRITY = "#BLELOG,INTEGRITY,boot=24,storage_errors=0,unclassified=0,overflow=0"
STATUS = "#BLELOG,CAN_STATUS,boot=24,status=1,code=34,name=NO_DATA,count=3"
SUMMARY = "#BLELOG,PEER_SUMMARY,boot=24,overflow=0"
DISCONNECT = ("#BLELOG,PEER_DISCONNECT,boot=24,peer_suffix=ABCD,id=7,"
              "gen=1,count=1,first_ms=180,last_ms=180,"
              "first_reason=0,last_reason=0")
FAULT = ("#BLELOG,FAULT,boot=24,code=21,name=BleDisconnected,"
         "count=1,first_ms=180,last_ms=180,first_detail=0,last_detail=0")
CONNECT = ("#BLELOG,EVENT,boot=24,index=0,seq=1,ms=100,kind=0,id=7,"
           "gen=1,peer_suffix=ABCD,reason_known=0,reason_raw=0,"
           "mtu=23,interval=0,latency=0,supervision=0,rx_bytes=0,"
           "tx_bytes=0,retries=0,failed=0,code=0,error=NONE,detail=0")
DISCONNECT_EVENT = ("#BLELOG,EVENT,boot=24,index=1,seq=2,ms=180,kind=1,id=7,"
                    "gen=1,peer_suffix=ABCD,reason_known=1,reason_raw=0,"
                    "mtu=23,interval=0,latency=0,supervision=0,rx_bytes=1024,"
                    "tx_bytes=4096,retries=0,failed=0,code=21,"
                    "error=BleDisconnected,detail=0")


def dump():
    return ("\n".join([BOOT, INTEGRITY, STATUS, SUMMARY, DISCONNECT, FAULT,
                       CONNECT, DISCONNECT_EVENT, "#BLELOG,END"])+"\n").encode()


class BleLogAuditorTests(unittest.TestCase):
    def bad(self, value, text):
        with self.assertRaisesRegex(EvidenceError, text):
            audit_blelog(value)

    def test_realistic_dual_peer_evidence_stays_partial(self):
        raw = dump()
        report = audit_blelog(raw)
        self.assertEqual(report["boot"], 24)
        self.assertEqual(report["events_total"], 2)
        self.assertEqual(report["events_kept"], 2)
        self.assertEqual(report["can_status_histogram"], {"1": 3})
        self.assertEqual(report["source_sha256"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(report["acceptance"], "SPARSE_BLE_JOURNAL_NOT_HARDWARE_APPROVAL")
        self.assertFalse(report["evidence_completeness"]["all_ble_events"])
        self.assertIn("RECORDED_BLE_OR_CAN_FAULTS", report["warnings"])

    def test_ring_overflow_generation_loss_never_looks_complete(self):
        src = dump().replace(b"total=2,kept=2,evicted=0",
                             b"total=14,kept=2,evicted=12")
        src = src.replace(b"queue_drops=0,seq_gaps=0",
                          b"queue_drops=4,seq_gaps=2")
        report = audit_blelog(src)
        self.assertIn("BLE_EVENT_LOSS_OR_RING_EVICTION", report["warnings"])
        self.assertEqual(report["events_evicted"], 12)

    def test_missing_final_marker_and_unknown_schema(self):
        self.bad(dump().replace(b"#BLELOG,END\n", b""), "missing END")
        self.bad(dump().replace(b"version=3", b"version=10"),
                 "unsupported BLELOG version")
        self.bad(dump().replace(b"#BLELOG,INTEGRITY,", b"#BLELOG,WHAT,"),
                 "unsupported row kind")

    def test_event_index_and_sequence_and_generation(self):
        self.bad(dump().replace(b"index=1", b"index=2"), "event index")
        self.bad(dump().replace(b"index=1,seq=2", b"index=1,seq=1"),
                 "sequence/time regressed")
        self.bad(dump().replace(b"index=1,seq=2", b"index=1,seq=3"),
                 "last BLE event sequence")
        self.bad(dump().replace(b"kind=1,id=7,gen=1", b"kind=1,id=7,gen=0"),
                 "lacks BLE generation")
        self.bad(dump().replace(b"ms=180,kind=1", b"ms=99,kind=1"),
                 "sequence/time regressed")

    def test_missing_rows_and_count_mismatch(self):
        self.bad(dump().replace(INTEGRITY.encode()+b"\n", b""),
                 "missing mandatory INTEGRITY")
        self.bad(dump().replace(DISCONNECT_EVENT.encode()+b"\n", b""),
                 "missing or extra journal EVENT")
        self.bad(dump().replace(b"total=2,kept=2,evicted=0",
                                b"total=2,kept=13,evicted=0"),
                 "inconsistent bounded BLE event retention")
        self.bad(dump().replace(b"#BLELOG,PEER_SUMMARY,", b"#BLELOG,ERROR,"),
                 "unsupported row kind")

    def test_fault_and_storage_counters_are_warnings(self):
        src = dump().replace(b"storage_errors=0", b"storage_errors=1")
        src = src.replace(b"overflow=0\n#BLELOG,PEER_DISCONNECT",
                          b"overflow=1\n#BLELOG,PEER_DISCONNECT")
        report = audit_blelog(src)
        self.assertIn("BLE_NVS_STORAGE_OR_UNKNOWN_FAULT", report["warnings"])
        self.assertIn("PEER_DISCONNECT_SUMMARY_OVERFLOW", report["warnings"])

    def test_nonpersistent_record_is_explicit(self):
        src = dump().replace(b"state=persisted", b"state=RAM_ONLY")
        report = audit_blelog(src)
        self.assertIn("EVENTS_NOT_CONFIRMED_PERSISTED", report["warnings"])

    def test_timestamp_and_type_mismatches(self):
        self.bad(dump().replace(b"last_ms=180", b"last_ms=20"),
                 "peer disconnect timestamp reversed")
        self.bad(dump().replace(b"kind=1,id=7,gen=1",
                                b"kind=9,id=7,gen=1"),
                 "invalid BLE event type")
        self.bad(dump().replace(b"status=1,code=34",
                                b"status=11,code=34"),
                 "duplicate/invalid CAN status")

    def test_legacy_v2_record_is_inspected_not_promoted(self):
        result = audit_blelog(dump().replace(b"version=3", b"version=2"))
        self.assertEqual(result["firmware_journal_version"], 2)
        self.assertFalse(result["evidence_completeness"]["per_query_notification_delivery"])


if __name__ == "__main__":
    unittest.main()
