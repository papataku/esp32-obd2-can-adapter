#!/usr/bin/env python3
"""Compare a legacy Ring V1 failure against the new Ring V2 + M5 GAP success.

Pure offline analysis: reads existing private RAW but never operates hardware.
It does not assume Ring and M5 clocks are synchronized, infer RF root cause,
or use a boot-wide trace seq as a count of user connection commands.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

from analyze_ble_linkedge_single import analyze, extract, endpoints

V1_PREFIX = "RING_EST_TRACE_V1 "
V1_FIELDS = ("seq", "path", "type", "api_seen", "api_rc", "api_dt_ms",
             "gap_seen", "gap_status", "gap_dt_ms", "duplicates",
             "final_rc", "linked", "overflow")
NUMBER = re.compile(r"-?[0-9]+\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")


def parse_v1(raw: bytes):
    complete = []
    invalid = 0
    for number, data in enumerate(raw.splitlines(keepends=True), 1):
        if not data.startswith(V1_PREFIX.encode()):
            continue
        if not data.endswith(b"\n"):
            invalid += 1
            continue
        try:
            line = data.decode("ascii").rstrip("\r\n")
        except UnicodeDecodeError:
            invalid += 1
            continue
        if not line.endswith(" END"):
            invalid += 1
            continue
        vals = {}
        for part in line[len(V1_PREFIX):-4].split():
            if "=" not in part:
                continue
            key, value = part.split("=", 1)
            if key in vals:
                vals = {}
                break
            vals[key] = value
        if not vals or any(key not in vals for key in V1_FIELDS):
            invalid += 1
            continue
        if any(not NUMBER.fullmatch(vals[k]) for k in V1_FIELDS if k != "path"):
            invalid += 1
            continue
        rec = {"source_line": number}
        rec.update({k: vals[k] if k == "path" else int(vals[k])
                    for k in V1_FIELDS})
        complete.append(rec)
    return complete, invalid


def compare(failure_ring: bytes, failure_m5: bytes,
            success_ring: bytes, success_m5: bytes):
    older, incomplete = parse_v1(failure_ring)
    recent = analyze(success_ring, success_m5)
    m5rows, m5invalid = extract(failure_m5, ("dual",))
    old_dual = endpoints(m5rows["dual"], ("clients", "connects",
                "disconnects", "rx_overflow", "notify", "retries", "failed"))
    new_edge = recent["m5"]["gap"]
    new_dual = recent["m5"]["dual"]

    warnings = [
        "Old Ring V1 has no DISCONNECT/MTU instrumentation: absent is unknown, not zero.",
        "M5 GAP observer was introduced only for the newer trial; do not backfill old M5 GAP state.",
        "M5 firmware and its boot counters differ between trials: do not subtract cross-boot counts.",
        "BLE trace seq is boot-wide, not a count of user commands; verify commands separately.",
        "Ring and M5 monotonic clocks are not synchronized.",
        "Queued M5 notifications are not radio-level delivery acknowledgements.",
        "One successful trial is not evidence that intermittent 574 is resolved.",
    ]
    anomalies = []
    if len(older) > 1:
        anomalies.append("multiple_complete_legacy_v1_records")
    if any(x["duplicates"] != 0 or x["overflow"] != 0 for x in older):
        anomalies.append("legacy_trace_duplicate_or_overflow")
    if old_dual and any(x["clients"] < 0 or x["clients"] > 2
                        for x in m5rows["dual"]):
        anomalies.append("legacy_m5_peer_limit_exceeded")
    if recent["anomalies"]:
        anomalies.extend("new_" + a for a in recent["anomalies"])

    f = older[0] if len(older) == 1 else None
    s = recent["ring"]["complete_v2"][0] if (
        len(recent["ring"]["complete_v2"]) == 1) else None
    comparison = None
    if f and s:
        comparison = {
            "ring_gap_api_return": {"legacy_failure": f["api_rc"],
                                    "new_success": s["api"]},
            "ring_gap_connect_status": {"legacy_failure": f["gap_status"],
                                        "new_success": s["gap"]},
            "ring_connected": {"legacy_failure": f["linked"],
                               "new_success": s["linked"]},
            "ring_final_error": {"legacy_failure": f["final_rc"],
                                 "new_success": s["final"]},
            "legacy_v1_mtu_api_return": "NOT_INSTRUMENTED",
            "legacy_v1_mtu_event": "NOT_INSTRUMENTED",
            "legacy_v1_early_disconnect": "NOT_INSTRUMENTED",
            "new_v2_mtu_api_return": s["mtu_api"],
            "new_v2_mtu_event": s["mtu_event"],
            "new_v2_disconnect_reason": s["disc"],
            "m5_gap_connect_failure_trial": "NOT_INSTRUMENTED",
            "m5_gap_new_trial": new_edge,
        }

    old_fail = (f is not None and f["api_seen"] == 1 and
                f["api_rc"] == 0 and f["gap_seen"] == 1 and
                f["gap_status"] == 0 and f["linked"] == 0 and
                f["final_rc"] == 574)
    new_success = (recent["classification"] ==
                   "RING_SINGLE_CONNECT_SUCCESS_ONLY")
    m5_consistent = (
        old_dual is not None and old_dual["first"]["clients"] == 0
        and old_dual["last"]["clients"] == 0
        and old_dual["first"]["connects"] == old_dual["last"]["connects"]
        and old_dual["first"]["disconnects"] == old_dual["last"]["disconnects"]
        and new_edge is not None and new_dual is not None
        and new_edge["first"]["con"] == 0
        and new_edge["last"]["con"] == 1
        and new_edge["last"]["ok"] == 1
        and new_edge["last"]["err"] == 0
        and new_edge["last"]["disc"] == 0
        and new_dual["first"]["clients"] == 0
        and new_dual["last"]["clients"] == 1
        and new_dual["last"]["connects"] -
        new_dual["first"]["connects"] == 1
    )
    status = "INCOMPLETE_OR_NONMATCHING_EVIDENCE"
    if old_fail and new_success and m5_consistent:
        status = "V1_574_VS_V2_SUCCESS_CONTRAST_ONLY"
    if incomplete or m5invalid["dual"] or recent["ring"]["incomplete_v2"]:
        status = "INCOMPLETE_EVIDENCE"
    if any(recent["m5"]["incomplete"].values()):
        status = "INCOMPLETE_EVIDENCE"
    if anomalies:
        status = "STOP_ANOMALY"

    return {
        "classification": status,
        "sha256": {
            "legacy_failure_ring": hashlib.sha256(failure_ring).hexdigest(),
            "legacy_failure_m5": hashlib.sha256(failure_m5).hexdigest(),
            "new_success_ring": hashlib.sha256(success_ring).hexdigest(),
            "new_success_m5": hashlib.sha256(success_m5).hexdigest(),
        },
        "legacy_failure": {
            "ring_v1_count": len(older), "ring_v1_invalid": incomplete,
            "ring_v1_trace": f,
            "m5_dual": old_dual,
            "m5_dual_invalid": m5invalid["dual"],
        },
        "new_success": {
            "ring_v2_count": len(recent["ring"]["complete_v2"]),
            "ring_v2_trace": s,
            "m5_gap": new_edge,
            "m5_dual": new_dual,
        },
        "comparison": comparison,
        "error_decode_if_574": {
            "nimble_value_decimal": 574,
            "nimble_value_hex": "0x23e",
            "hci_error_hex": "0x3e",
            "symbol": "BLE_ERR_CONN_ESTABLISHMENT",
            "meaning": "Connection Failed to be Established",
            "root_cause": "NOT_DETERMINED",
        } if f and f["final_rc"] == 574 else None,
        "anomalies": sorted(set(anomalies)),
        "warnings": warnings,
    }


def require_digest(name: str, raw: bytes, expected: str):
    expected = expected.lower()
    if not SHA256.fullmatch(expected):
        raise ValueError("invalid expected SHA256 for " + name)
    actual = hashlib.sha256(raw).hexdigest()
    if actual != expected:
        raise ValueError("SHA256 mismatch for " + name +
                         " (no comparison report written)")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for key in ("failure-ring", "failure-m5", "success-ring", "success-m5"):
        ap.add_argument("--" + key, type=Path, required=True)
        ap.add_argument("--expected-" + key + "-sha256", required=True)
    ap.add_argument("--output", type=Path)
    args = ap.parse_args()
    sources = {
        "failure_ring": args.failure_ring.read_bytes(),
        "failure_m5": args.failure_m5.read_bytes(),
        "success_ring": args.success_ring.read_bytes(),
        "success_m5": args.success_m5.read_bytes(),
    }
    for name, raw in sources.items():
        require_digest(name, raw, getattr(args, "expected_" + name + "_sha256"))
    result = compare(**sources)
    payload = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(payload, encoding="utf-8")
    else:
        print(payload, end="")
    if result["classification"] != "V1_574_VS_V2_SUCCESS_CONTRAST_ONLY":
        raise SystemExit(2)


if __name__ == "__main__":
    main()
