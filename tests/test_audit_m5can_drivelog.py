#!/usr/bin/env python3
"""All #DRIVELOG fixtures are synthetic and not hardware qualification."""
import hashlib
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from audit_m5can_drivelog import audit_drivelog, EvidenceError

HEAD = "#DRIVELOG,BEGIN,version=2,kind=1:enabled/2:fallback/3:stopped/4:disconnect/5:lease_denied/6:can_fault"
BOOT = ("#DRIVELOG,boot=24,reset=0,rom_reset=0,build=0.4.4-phase3f-demo-touch,"
        "multipid=1,uptime_ms=120000,saves=4,grouped_submissions=2,"
        "individual_submissions=15,group_ok=1,fallback=1,stopped=0,"
        "disconnects=2,demo=0,clients=0,peers=0000/0000,fault=0,lease_ms=0")
CAN = ("#DRIVELOG,CAN,boot=24,rx=60,tx=15/18,q=18,ok=15,no_data=2,"
       "failed=1,timeout=1,drop=0,buserr=0,missed=0,overrun=0,"
       "BLE_rx_overflow=0,notify_failed=0,retries=1")
TIMING = "#DRIVELOG,TIMING,boot=24,group_outcomes=2,group_total_us=140000,group_max_us=100000"
MEMORY = "#DRIVELOG,MEMORY,boot=24,can_stack_free_bytes=1000,ble_stack_free_bytes=1500,heap_min=10000,largest_heap=9000"
COORD = ("#DRIVELOG,COORD,boot=24,sampled_ms=1000,cache_hits=1,"
         "cache_lookup_misses=2,joins=3,grouped_submissions=1,"
         "cross_group_submissions=0,single_submissions=2,owner_cache_hits=0")
PROBE = ("#DRIVELOG,PROBE,boot=24,state=persisted,ms=200,decision=1,"
         "reason=0,status=0,ecu=18DAF101,responses=1,raw_count=1,"
         "raw_saved=1,truncated=0,transaction_us=25000,"
         "first_response_us=10000,rx_time=tx_offset_us,origin=CAN")
PROBE_RAW = ("#DRIVELOG,PROBE_RAW,boot=24,index=0,id=18DAF101,dlc=5,"
             "rx_tx_us=10000,data=04622012AA")
EVENT = ("#DRIVELOG,EVENT,boot=24,ms=100,kind=4,peer=7,status=0,"
         "ecu=18DAF101,responses=1,raw_count=1,transaction_us=25000,"
         "first_response_us=10000")
RAW = "#DRIVELOG,RAW,boot=24,event=0,id=18DAF101,dlc=5,data=04622012AA"


def capture(*, probe=True, event=True, coord=True):
    items = [HEAD, BOOT, CAN, TIMING, MEMORY]
    if coord:
        items.append(COORD)
    if probe:
        items.extend((PROBE, PROBE_RAW))
    if event:
        items.extend((EVENT, RAW))
    return ("\n".join(items + ["#DRIVELOG,END"]) + "\n").encode()


class DriveLogAuditTests(unittest.TestCase):
    def rejects(self, source, pattern):
        with self.assertRaisesRegex(EvidenceError, pattern):
            audit_drivelog(source)

    def test_valid_sparse_journal_reports_missing_evidence_not_approval(self):
        raw = capture()
        result = audit_drivelog(raw)
        self.assertEqual(result["source_sha256"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(result["boot_count"], 1)
        self.assertEqual(result["boots"][0]["query_count"], 18)
        self.assertEqual(result["boots"][0]["can_tx_attempts"], 18)
        self.assertEqual(result["boots"][0]["can_tx_success"], 15)
        self.assertEqual(result["boots"][0]["recorded_state_events"], 1)
        self.assertFalse(result["evidence_completeness"]["independent_can_analyzer"])
        self.assertFalse(result["evidence_completeness"]["per_query_ble_delivery_provenance"])
        self.assertEqual(result["acceptance"], "PARTIAL_JOURNAL_ONLY_NOT_HARDWARE_APPROVAL")

    def test_missing_probe_is_marked_gap(self):
        raw = capture(probe=False).replace(
            MEMORY.encode() + b"\n",
            (MEMORY + "\n#DRIVELOG,PROBE,boot=24,state=MISSING\n").encode()
        )
        result = audit_drivelog(raw)
        self.assertIn("boot=24:PROBE:MISSING", result["warnings"])

    def test_synthetic_and_truncated_probe_not_qualified(self):
        source = capture()
        source = source.replace(b"origin=CAN", b"origin=SYNTHETIC")
        source = source.replace(b"raw_count=1,raw_saved=1,truncated=0",
                                b"raw_count=3,raw_saved=1,truncated=1")
        result = audit_drivelog(source)
        self.assertIn("boot=24:PROBE:SYNTHETIC", result["warnings"])
        self.assertIn("boot=24:PROBE:TRUNCATED", result["warnings"])

    def test_transport_counters_warn_without_being_a_pass(self):
        result = audit_drivelog(capture().replace(b"drop=0", b"drop=12"))
        self.assertTrue(any("ERROR_COUNTERS" in x for x in result["warnings"]))

    def test_gapped_raw_evidence_is_rejected(self):
        self.rejects(capture().replace(PROBE_RAW.encode() + b"\n", b""),
                     "actual RAW count")
        self.rejects(capture().replace(b"index=0", b"index=1"),
                     "RAW frame indices")
        self.rejects(capture().replace(b"dlc=5,data=04622012AA", b"dlc=4,data=04622012AA"),
                     "RAW corrupt CAN payload")
        self.rejects(capture().replace(b"id=18DAF101,dlc=5", b"id=INVALID!,dlc=5"),
                     "invalid hex id")

    def test_avoid_false_success_on_partial_capture(self):
        self.rejects(capture()[:-len("#DRIVELOG,END\n")], "missing END marker")
        self.rejects(capture().replace(b"BEGIN,version=2", b"BEGIN,version=3"),
                     "version 2 BEGIN")
        self.rejects(capture().replace(b"#DRIVELOG,CAN,", b"#DRIVELOG,UNKNOWN,"),
                     "unknown journal entry")
        self.rejects(capture().replace(b"tx=15/18", b"tx=19/18"),
                     "successful TX > attempts")
        self.rejects(capture().replace(MEMORY.encode() + b"\n", b""),
                     "mandatory CAN/TIMING/MEMORY")

    def test_boot_association_and_duplicate_rejected(self):
        self.rejects(capture().replace(b"#DRIVELOG,CAN,boot=24",
                                      b"#DRIVELOG,CAN,boot=23"), "wrong boot")
        self.rejects(capture().replace(BOOT.encode() + b"\n",
                                      BOOT.encode() + b"\n" + BOOT.encode() + b"\n"),
                     "duplicate boot")

    def test_short_empty_or_garbage_dump_fails(self):
        self.rejects(b"", "empty")
        self.rejects(b"#DRIVELOG,ERR,ACTIVE_TRAFFIC\n", "version 2 BEGIN")
        self.rejects(capture() + b"noise\n", "missing END marker")
        self.rejects(capture().replace(b"CAN,boot", b"CAN,boot=24,junk=val,boot"),
                     "empty/duplicate")

    def test_dry_dump_without_probe_and_optional_coord(self):
        result = audit_drivelog(capture(probe=False, event=False, coord=False))
        self.assertEqual(result["boots"][0]["recorded_state_events"], 0)
        self.assertEqual(result["boots"][0]["recorded_probe_kinds"], [])
        self.assertEqual(result["warnings"], ["SPARSE_JOURNAL_ONLY"])


if __name__ == "__main__":
    unittest.main()
