#!/usr/bin/env python3
"""Audit USB DRYTRACE from TX-disabled two-BLE firmware.

Covers generation ownership, trace loss, CAN-owner result and BLE enqueue.
It cannot verify physical CAN TX or on-air BLE delivery. Source is *not*
accepted as an independent CAN analyzer recording.
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

from validate_multi_ecu_bench_trace import EvidenceError

EVENTS = {
    1:"CONNECT", 2:"DISCONNECT", 3:"CAN_QUEUE_ACCEPTED",
    4:"CAN_OWNER_RESULT", 5:"BLE_APP_QUEUE_ACCEPTED",
    6:"BLE_APP_QUEUE_REJECTED", 7:"BLE_NOTIFY_API_ACCEPTED",
    8:"BLE_NOTIFY_API_REJECTED", 9:"STALE_OWNER_DISCARD"
}


def check(value, message, n):
    if not value:
        raise EvidenceError(f"DRYTRACE line {n}: {message}")


def parse_kv(items, n):
    result = {}
    for item in items:
        check("=" in item, "invalid key=value", n)
        key, value = item.split("=", 1)
        check(bool(key) and bool(value) and key not in result,
              "empty/duplicate field", n)
        result[key] = value
    return result


def dec(row, key, n):
    value = row.get(key, "")
    check(value.isdigit(), f"missing/nonnegative decimal {key}", n)
    return int(value)


def hex8(row, key, n):
    val = row.get(key, "")
    check(len(val)==8 and all(x in "0123456789ABCDEF" for x in val),
          f"{key} requires 8 uppercase hexadecimal digits", n)
    return int(val,16)


def audit(blob):
    try:
        rows=[(i,line.strip()) for i,line in
              enumerate(blob.decode("utf-8").splitlines(),1) if line.strip()]
    except UnicodeError as err:
        raise EvidenceError("DRYTRACE not UTF-8") from err
    check(len(rows)>=2 and rows[0][1].startswith("#DRYTRACE,BEGIN,"),
          "missing BEGIN",1)
    check(rows[-1][1]=="#DRYTRACE,END","missing END",rows[-1][0])
    header=parse_kv(rows[0][1].split(",")[2:],rows[0][0])
    check(dec(header,"version",1)==1,"unknown version",1)
    check(header.get("origin")=="TX_DISABLED_BENCH",
          "not an explicitly TX-disabled profile",1)
    check(header.get("wire_can_tx_proven")=="0" and
          header.get("ble_peer_delivery_proven")=="0",
          "trace must not assert physical CAN TX or peer receipt",1)
    capacity=dec(header,"capacity",1)
    total=dec(header,"total",1)
    kept=dec(header,"kept",1)
    overwritten=dec(header,"overwritten",1)
    check(0<capacity<=4096 and kept<=capacity and kept<=total and
          overwritten==total-kept,"inconsistent ring counts",1)

    incomplete=overwritten>0
    live={}
    generations={}
    requests={}
    last_t=-1
    event_counts={v:0 for v in EVENTS.values()}
    for i,(number,line) in enumerate(rows[1:-1]):
        parts=line.split(",")
        check(len(parts)>=3 and parts[:2]==["#DRYTRACE","EVENT"],
              "unexpected event row",number)
        row=parse_kv(parts[2:],number)
        check(dec(row,"index",number)==i,"event index gap",number)
        timestamp=dec(row,"t_us",number)
        check(timestamp>=last_t,"timestamp regression",number)
        last_t=timestamp
        kind=dec(row,"kind",number)
        check(kind in EVENTS,f"unknown event kind {kind}",number)
        event_counts[EVENTS[kind]]+=1
        token=dec(row,"token",number)
        conn=dec(row,"conn",number)
        gen=dec(row,"gen",number)
        canid=hex8(row,"can_id",number)
        hex8(row,"identity",number)
        dec(row,"bytes",number)
        dec(row,"status",number)
        if kind==1:
            check(gen>=1,"connected peer lacks generation",number)
            check(conn not in live,"overlapping active connection handle",number)
            if conn in generations:
                check(gen>generations[conn],"reused BLE generation",number)
            generations[conn]=gen
            live[conn]=gen
        elif kind==2:
            if live.get(conn)!=gen:
                check(incomplete,"disconnect without matching connect",number)
            elif conn in live:
                del live[conn]
        elif kind==3:
            check(token and token not in requests,"duplicate/zero CAN query token",number)
            check(canid and gen,"CAN queue missing ECU/generation",number)
            check(live.get(conn)==gen or incomplete,
                  "CAN queue assigned disconnected generation",number)
            requests[token]={"conn":conn,"gen":gen,"can_id":canid,
                             "result":False,"queued_peers":set()}
        elif kind==4:
            if token not in requests:
                check(incomplete,"CAN result without matching queued request",number)
            else:
                info=requests[token]
                check(not info["result"],"duplicate CAN owner result",number)
                check((conn,gen,canid)==
                      (info["conn"],info["gen"],info["can_id"]),
                      "CAN result misattributed to request owner/ECU",number)
                info["result"]=True
        elif kind==9:
            check(token in requests or incomplete,
                  "stale-owner discard without CAN request token",number)
        elif kind in (5,6):
            check(gen>=1,"BLE queue event missing generation",number)
            if token:
                if token not in requests:
                    check(incomplete,"BLE queue refers to unknown request",number)
                else:
                    info=requests[token]
                    check(info["result"],
                          "BLE queue for request before CAN owner result",number)
                    check(info["can_id"]==canid,
                          "BLE queue assigned another ECU",number)
                    check(live.get(conn)==gen or incomplete,
                          "BLE queue belongs to disconnected generation",number)
                    info["queued_peers"].add((conn,gen))
        elif kind in (7,8):
            check(token==0,"notify API event has invented request identity",number)
            check(gen>=1,"notify API event missing generation",number)
            check(live.get(conn)==gen or incomplete,
                  "notify API handoff to disconnected generation",number)

    check(len(rows)-2==kept,"kept count differs from observed EVENT rows",1)
    issues=[]
    if overwritten:
        issues.append(f"TRACE_RING_OVERWRITTEN={overwritten}")
    for token,info in requests.items():
        if not info["result"]:
            issues.append(f"CAN_RESULT_NOT_IN_CAPTURE_TOKEN={token}")
    return {
        "kind":"m5can_dry_bench_trace_audit",
        "schema":1,
        "source_sha256":hashlib.sha256(blob).hexdigest(),
        "events_total":total,"events_kept":kept,
        "events_overwritten":overwritten,
        "correlated_request_tokens":len(requests),
        "event_counts":event_counts,
        "warnings":issues,
        "evidence_completeness":{
            "physical_can_tx_observed":False,
            "independent_can_analyzer":False,
            "actual_ble_peer_receipt":False,
            "complete_event_history":not incomplete,
        },
        "acceptance":"DRY_RUN_SOFTWARE_TRACE_ONLY_NOT_VEHICLE_APPROVAL",
    }


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("original_trace",type=Path)
    p.add_argument("output_report",type=Path)
    args=p.parse_args()
    try:
        check(args.original_trace.resolve()!=args.output_report.resolve(),
              "cannot overwrite source",1)
        report=audit(args.original_trace.read_bytes())
        with args.output_report.open("x",encoding="utf-8") as fh:
            json.dump(report,fh,sort_keys=True,indent=2)
            fh.write("\n")
    except (EvidenceError,OSError,ValueError) as err:
        print(f"FAIL: {err}",file=sys.stderr)
        return 1
    print("PASS DRY BENCH TRACE STRUCTURE (NOT VEHICLE APPROVAL): "+
          json.dumps(report,sort_keys=True))
    return 0


if __name__=="__main__":
    raise SystemExit(main())
