#!/usr/bin/env python3
"""SYNTHETIC DRYTRACE fixtures only; no actual CAN or BLE hardware."""
import hashlib
import sys
import unittest
from pathlib import Path

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from audit_m5can_drytrace import audit, EvidenceError

def event(index,t,kind,token=0,conn=0,gen=0,can_id="00000000",
          identity="00000000",bytes_=0,status=0):
    return (f"#DRYTRACE,EVENT,index={index},t_us={t},kind={kind},"
            f"token={token},conn={conn},gen={gen},can_id={can_id},"
            f"identity={identity},bytes={bytes_},status={status}")

def dump(lines,*,total=None,overwritten=0):
    n=len(lines)
    if total is None:total=n+overwritten
    head=(f"#DRYTRACE,BEGIN,version=1,origin=TX_DISABLED_BENCH,"
          f"capacity=128,total={total},kept={n},overwritten={overwritten},"
          f"wire_can_tx_proven=0,ble_peer_delivery_proven=0")
    return ("\n".join([head]+lines+["#DRYTRACE,END"])+"\n").encode()

def good():
    return [
        event(0,100,1,conn=7,gen=1),
        event(1,110,1,conn=8,gen=1),
        event(2,120,3,token=1,conn=7,gen=1,can_id="18DA01F1",
              identity="00032220"),
        event(3,140,4,token=1,conn=7,gen=1,can_id="18DA01F1"),
        event(4,150,5,token=1,conn=7,gen=1,can_id="18DA01F1",bytes_=80),
        event(5,155,5,token=1,conn=8,gen=1,can_id="18DA01F1",bytes_=80),
        event(6,160,7,conn=7,gen=1,bytes_=20),
        event(7,165,7,conn=8,gen=1,bytes_=20),
        event(8,200,2,conn=7,gen=1),
        event(9,205,2,conn=8,gen=1),
    ]

class DryTraceTests(unittest.TestCase):
    def bad(self,data,pattern):
        with self.assertRaisesRegex(EvidenceError,pattern):
            audit(data)

    def test_two_peer_replies_prove_only_enqueue(self):
        original=dump(good())
        report=audit(original)
        self.assertEqual(report["source_sha256"],hashlib.sha256(original).hexdigest())
        self.assertEqual(report["correlated_request_tokens"],1)
        self.assertEqual(report["event_counts"]["BLE_NOTIFY_API_ACCEPTED"],2)
        self.assertFalse(report["evidence_completeness"]["physical_can_tx_observed"])
        self.assertFalse(report["evidence_completeness"]["actual_ble_peer_receipt"])
        self.assertEqual(report["warnings"],[])
        self.assertEqual(report["acceptance"],
                         "DRY_RUN_SOFTWARE_TRACE_ONLY_NOT_VEHICLE_APPROVAL")

    def test_disconnect_handle_reuse_and_stale_result(self):
        lines=[
            event(0,1,1,conn=7,gen=1),
            event(1,2,3,token=99,conn=7,gen=1,can_id="18DA01F1"),
            event(2,3,2,conn=7,gen=1,token=99),
            event(3,4,1,conn=7,gen=2),
            event(4,5,4,token=99,conn=7,gen=1,can_id="18DA01F1"),
            event(5,6,9,token=99,conn=7,gen=1,can_id="18DA01F1"),
            event(6,7,2,conn=7,gen=2),
        ]
        self.assertEqual(audit(dump(lines))["event_counts"]["STALE_OWNER_DISCARD"],1)

    def test_source_rejects_unsupported_profile_or_fake_proofs(self):
        d=dump(good())
        self.bad(d.replace(b"TX_DISABLED_BENCH",b"TX_ALLOWED"),"TX-disabled")
        self.bad(d.replace(b"wire_can_tx_proven=0",b"wire_can_tx_proven=1"),
                 "must not assert physical")
        self.bad(d.replace(b"ble_peer_delivery_proven=0",
                           b"ble_peer_delivery_proven=1"),"must not assert physical")
        self.bad(d[:-len("#DRYTRACE,END\n")],"missing END")
        self.bad(d.replace(b"version=1",b"version=2"),"unknown version")

    def test_event_sequence_and_timestamp_fail_closed(self):
        lines=good()
        lines[4]=lines[4].replace("index=4","index=42")
        self.bad(dump(lines),"event index")
        lines=good()
        lines[4]=lines[4].replace("t_us=150","t_us=100")
        self.bad(dump(lines),"timestamp regression")
        lines=good()
        lines[2]=lines[2].replace("kind=3","kind=99")
        self.bad(dump(lines),"unknown event kind")
        lines=good()
        lines[2]=lines[2].replace("can_id=18DA01F1","can_id=????")
        self.bad(dump(lines),"uppercase hexadecimal")

    def test_result_cannot_change_physical_ecu_or_generation(self):
        lines=good()
        lines[3]=lines[3].replace("can_id=18DA01F1","can_id=18DA02F1")
        self.bad(dump(lines),"CAN result misattributed")
        lines=good()
        lines[3]=lines[3].replace("gen=1","gen=2")
        self.bad(dump(lines),"CAN result misattributed")
        lines=good()
        lines[3]=lines[3].replace("token=1","token=32")
        self.bad(dump(lines),"CAN result without matching queued")

    def test_no_ble_queue_from_other_ecu_or_before_can_result(self):
        lines=good()
        lines[4]=lines[4].replace("can_id=18DA01F1","can_id=18DA06F1")
        self.bad(dump(lines),"BLE queue assigned another ECU")
        lines=good()
        lines[3],lines[4]=lines[4],lines[3]
        lines[3]=lines[3].replace("index=4","index=3").replace("t_us=150","t_us=140")
        lines[4]=lines[4].replace("index=3","index=4").replace("t_us=140","t_us=150")
        self.bad(dump(lines),"BLE queue for request before CAN owner result")

    def test_duplicate_or_reused_generation_rejected(self):
        lines=good()
        lines[1]=lines[1].replace("conn=8","conn=7")
        self.bad(dump(lines),"overlapping active connection")
        lines=good()+[event(10,210,1,conn=7,gen=1)]
        self.bad(dump(lines),"reused BLE generation")

    def test_notify_api_does_not_invent_request_token(self):
        lines=good()
        lines[6]=lines[6].replace("token=0","token=1")
        self.bad(dump(lines),"notify API event has invented")
        lines=good()
        lines[6]=lines[6].replace("gen=1","gen=2")
        self.bad(dump(lines),"notify API handoff to disconnected")

    def test_overwrite_explicitly_prevents_full_history_claim(self):
        # Original events may have been overwritten. Missing CONNECT/REQUEST
        # does not constitute positive attribution.
        lines=[event(0,100,4,token=20,conn=7,gen=5,can_id="18DA01F1")]
        report=audit(dump(lines,total=130,overwritten=129))
        self.assertEqual(report["events_overwritten"],129)
        self.assertFalse(report["evidence_completeness"]["complete_event_history"])
        self.assertIn("TRACE_RING_OVERWRITTEN=129",report["warnings"])
        self.bad(dump(good(),total=20,overwritten=0),"inconsistent ring counts")

    def test_incomplete_inflight_request_reported(self):
        lines=good()[:3]
        result=audit(dump(lines))
        self.assertIn("CAN_RESULT_NOT_IN_CAPTURE_TOKEN=1",result["warnings"])

    def test_no_ble_queue_to_disconnected_generation(self):
        lines=good()
        lines[4]=lines[4].replace("gen=1","gen=2")
        self.bad(dump(lines),"BLE queue belongs to disconnected generation")

if __name__=="__main__":
    unittest.main()
