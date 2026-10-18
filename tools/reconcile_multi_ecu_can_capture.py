#!/usr/bin/env python3
"""Cross-check CAN/BLE event evidence against an independently recorded CAN log.

This is an OFFLINE read-only test. No hardware access, CAN TX, or firmware
authorization is possible. Matching a generated fixture is NOT hardware proof.

The independent CAN analyzer must export canonical JSONL:
  {"type":"capture_meta","schema":1,"lost_frames":0}
  {"t_us":123456,"can_id":"18DA02F1","data_hex":"0322201200000000",
   "dlc":8,"extended":true,"rtr":false}

The event trace uses docs/MULTI_ECU_BENCH_EVIDENCE_SCHEMA.md (hardware
evidence; real payload bytes). Its metadata capture_sha256 must hash the exact
independent JSONL file bytes. The operator must measure/record the clock
alignment; a CLI offset is not proof of synchronized clocks.
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

from validate_multi_ecu_bench_trace import EvidenceError, verify

CAN_ID = re.compile(r"^[0-9A-F]{8}$")
CAN_DATA = re.compile(r"^(?:[0-9A-F]{2}){1,8}$")
TESTER_ID = re.compile(r"^18DA[0-9A-F]{2}F1$")
FUNCTIONAL_TESTER_IDS = {"18DB33F1", "18DBEFF1"}


def required(condition, message):
    if not condition:
        raise EvidenceError(message)


def diagnostic_tester(can_id):
    return bool(TESTER_ID.fullmatch(can_id)) or can_id in FUNCTIONAL_TESTER_IDS


def parse_raw_capture(data):
    """Validate *all* raw analyzer rows, never ignore malformed/unknown rows."""
    try:
        rows = [json.loads(line) for line in data.decode("utf-8").splitlines()
                if line.strip()]
    except (UnicodeError, ValueError) as exc:
        raise EvidenceError(f"invalid independent CAN JSONL: {exc}") from exc
    required(rows and type(rows[0]) is dict and
             rows[0].get("type") == "capture_meta" and
             rows[0].get("schema") == 1,
             "independent CAN capture metadata missing/unsupported")
    required(type(rows[0].get("lost_frames")) is int and
             rows[0]["lost_frames"] == 0,
             "independent CAN capture has missing/unknown frame-loss counter")
    last_time = -1
    output = []
    for line, frame in enumerate(rows[1:], 2):
        required(type(frame) is dict, f"CAN line {line}: not an object")
        t = frame.get("t_us")
        can_id = frame.get("can_id")
        payload = frame.get("data_hex")
        required(type(t) is int and t >= last_time,
                 f"CAN line {line}: invalid/regressed timestamp")
        required(type(can_id) is str and bool(CAN_ID.fullmatch(can_id)),
                 f"CAN line {line}: invalid 29-bit CAN ID representation")
        required(type(payload) is str and bool(CAN_DATA.fullmatch(payload)),
                 f"CAN line {line}: invalid Classical CAN data bytes")
        required(type(frame.get("dlc")) is int and
                 frame["dlc"] == len(payload) // 2,
                 f"CAN line {line}: DLC mismatch")
        required(frame.get("extended") is True and frame.get("rtr") is False,
                 f"CAN line {line}: expected extended data frame")
        output.append(frame)
        last_time = t
    required(output, "independent CAN capture has no frames")
    return output


def reconcile(trace, capture_bytes, *, clock_offset_us=0, max_skew_us=5000):
    """Return counts only on exact ordered, timestamp-bounded frame matching.

    clock_offset_us = independent_CAN_t_us - event_trace_t_us.
    No physical interface is opened; evidence is not cryptographically
    signed or authenticated, only hashed for integrity.
    """
    required(type(clock_offset_us) is int and type(max_skew_us) is int and
             0 <= max_skew_us <= 1_000_000,
             "invalid clock offset or maximum skew")
    summary = verify(trace)
    required(summary["evidence"] == "hardware",
             "reconciliation requires hardware-schema event rows with raw bytes")
    expected_hash = trace[0]["capture_sha256"].lower()
    actual_hash = hashlib.sha256(capture_bytes).hexdigest()
    required(expected_hash == actual_hash,
             "independent CAN capture SHA-256 differs from event metadata")
    frames = parse_raw_capture(capture_bytes)
    if summary["profile"] == "passive_tx_off":
        required(not any(diagnostic_tester(f["can_id"]) for f in frames),
                 "TX=OFF profile has physical diagnostic tester frames")
    events = [e for e in trace[1:] if e.get("kind") in {"can_tx", "can_rx"}]
    matched_indices = {}
    index = 0
    for event in events:
        expected_time = event["t_us"] + clock_offset_us
        found = None
        while index < len(frames):
            item = frames[index]
            if item["t_us"] < expected_time - max_skew_us:
                index += 1
                continue
            if item["t_us"] > expected_time + max_skew_us:
                break
            if all(item.get(key) == event.get(key) for key in
                   ("can_id", "data_hex", "dlc", "extended", "rtr")):
                found = index
                index += 1
                break
            index += 1
        required(found is not None,
                 f"independent CAN capture missing/out-of-order frame at "
                 f"trace t_us={event['t_us']} ID={event.get('can_id')}")
        matched_indices[found] = event["kind"]

    # An isolated bench has only our tester emitting 18DAxxF1 or known
    # functional headers. Any such frame absent from event log is a hard
    # failure, not ignorable unrelated bus noise.
    for ix, frame in enumerate(frames):
        if diagnostic_tester(frame["can_id"]):
            required(matched_indices.get(ix) == "can_tx",
                     f"unaccounted physical diagnostic TX on independent "
                     f"CAN capture at t_us={frame['t_us']}")

    return {
        "evidence": "hardware trace structure + independent file hash/frame match",
        "profile": summary["profile"],
        "sha256": actual_hash,
        "raw_frames": len(frames),
        "matched_frames": len(matched_indices),
        "unmatched_background_frames": len(frames) - len(matched_indices),
        "diagnostic_tx": sum(1 for f in frames if diagnostic_tester(f["can_id"])),
        "clock_offset_us": clock_offset_us,
        "max_skew_us": max_skew_us,
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("event_trace", type=Path)
    p.add_argument("independent_can_jsonl", type=Path)
    p.add_argument("--clock-offset-us", type=int, default=0,
                   help="independent CAN clock minus event trace clock")
    p.add_argument("--max-skew-us", type=int, default=5000)
    args = p.parse_args()
    try:
        trace = [json.loads(line) for line in
                 args.event_trace.read_text(encoding="utf-8").splitlines()
                 if line.strip()]
        result = reconcile(
            trace, args.independent_can_jsonl.read_bytes(),
            clock_offset_us=args.clock_offset_us,
            max_skew_us=args.max_skew_us)
    except (OSError, ValueError, EvidenceError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    print("PASS OFFLINE FRAME RECONCILIATION (NOT RELEASE APPROVAL): " +
          json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
