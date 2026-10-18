#!/usr/bin/env python3
"""Audit an existing #BLELOG NVS journal dump without claiming live BLE proof.

Only the most recent 12 diagnostic events are retained by firmware, with
boot-level fault counters and limited per-peer disconnect summaries. This
cannot prove every request's generation or actual notification delivery.
No BLE/serial/hardware access is used.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

from validate_multi_ecu_bench_trace import EvidenceError


def require(condition, message, line):
    if not condition:
        raise EvidenceError(f"BLELOG line {line}: {message}")


def attrs(items, line):
    result = {}
    for item in items:
        require("=" in item, "not key=value", line)
        key, value = item.split("=", 1)
        require(bool(key) and bool(value) and key not in result,
                "empty/duplicate field", line)
        result[key] = value
    return result


def integer(row, name, line, minimum=0):
    value = row.get(name, "")
    require(value.isdigit() and int(value) >= minimum,
            f"missing/invalid nonnegative {name}", line)
    return int(value)


def audit_blelog(original: bytes) -> dict:
    try:
        lines = original.decode("utf-8").splitlines()
    except UnicodeError as exc:
        raise EvidenceError("BLELOG input is not UTF-8") from exc
    rows = [(i, x.strip()) for i, x in enumerate(lines, 1) if x.strip()]
    require(bool(rows) and rows[0][1].startswith("#BLELOG,BOOT,"),
            "requires first BOOT line from USB BLELOG dump", 1)
    require(rows[-1][1] == "#BLELOG,END", "missing END marker", rows[-1][0])
    boot = attrs(rows[0][1].split(",")[2:], rows[0][0])
    version = integer(boot, "version", rows[0][0])
    require(version in (2, 3), "unsupported BLELOG version", rows[0][0])
    boot_number = integer(boot, "boot", rows[0][0], minimum=1)
    total = integer(boot, "total", rows[0][0])
    kept = integer(boot, "kept", rows[0][0])
    evicted = integer(boot, "evicted", rows[0][0])
    queue_drops = integer(boot, "queue_drops", rows[0][0])
    seq_gaps = integer(boot, "seq_gaps", rows[0][0])
    require(kept <= 12 and kept <= total and
            evicted == total - kept,
            "inconsistent bounded BLE event retention", rows[0][0])
    require(boot.get("state") in {"persisted", "RAM_ONLY"},
            "unknown journal persistence state", rows[0][0])
    require(bool(boot.get("build")), "firmware build missing", rows[0][0])

    integrity = None
    peer_summary = None
    statuses = []
    faults = []
    peers = []
    events = []
    warnings = []
    for number, line in rows[1:-1]:
        require(line.startswith("#BLELOG,"), "unexpected non-BLELOG row", number)
        parts = line.split(",")
        kind = parts[1]
        require(kind in {"INTEGRITY", "CAN_STATUS", "PEER_SUMMARY",
                         "PEER_DISCONNECT", "FAULT", "EVENT"},
                f"unsupported row kind {kind}", number)
        row = attrs(parts[2:], number)
        require(integer(row, "boot", number) == boot_number,
                "journal row from different boot", number)
        if kind == "INTEGRITY":
            require(integrity is None, "duplicate INTEGRITY", number)
            for field in ("storage_errors", "unclassified", "overflow"):
                integer(row, field, number)
            integrity = row
        elif kind == "PEER_SUMMARY":
            require(peer_summary is None, "duplicate PEER_SUMMARY", number)
            integer(row, "overflow", number)
            peer_summary = row
        elif kind == "CAN_STATUS":
            status = integer(row, "status", number)
            require(status < 10 and all(int(s["status"]) != status for s in statuses),
                    "duplicate/invalid CAN status", number)
            integer(row, "code", number)
            integer(row, "count", number, minimum=1)
            statuses.append(row)
        elif kind == "FAULT":
            code = integer(row, "code", number, minimum=1)
            require(all(int(f["code"]) != code for f in faults),
                    "duplicate fault bucket", number)
            count = integer(row, "count", number, minimum=1)
            first = integer(row, "first_ms", number)
            last = integer(row, "last_ms", number)
            require(last >= first, "fault last timestamp earlier than first", number)
            faults.append(row)
        elif kind == "PEER_DISCONNECT":
            integer(row, "id", number)
            integer(row, "gen", number, minimum=1)
            integer(row, "count", number, minimum=1)
            start = integer(row, "first_ms", number)
            stop = integer(row, "last_ms", number)
            require(stop >= start, "peer disconnect timestamp reversed", number)
            peers.append(row)
            require(len(peers) <= 4, "peer disconnect summaries exceed capacity", number)
        else:
            idx = integer(row, "index", number)
            require(idx == len(events), "event index out of order/gap", number)
            seq = integer(row, "seq", number)
            ms = integer(row, "ms", number)
            kind_id = integer(row, "kind", number)
            require(kind_id <= 3, "invalid BLE event type", number)
            integer(row, "id", number)
            gen = integer(row, "gen", number)
            if kind_id in (0, 1):
                require(gen > 0, "connect/disconnect lacks BLE generation", number)
            for field in ("reason_known", "code", "detail",
                          "rx_bytes", "tx_bytes", "retries", "failed"):
                integer(row, field, number)
            if events:
                require(seq > int(events[-1]["seq"]) and
                        ms >= int(events[-1]["ms"]),
                        "BLE event sequence/time regressed", number)
            events.append(row)

    require(integrity is not None and peer_summary is not None,
            "missing mandatory INTEGRITY or PEER_SUMMARY", 1)
    require(len(events) == kept, "missing or extra journal EVENT rows", 1)
    if kept:
        require(int(events[-1]["seq"]) == integer(boot, "last_seq", rows[0][0]),
                "last BLE event sequence differs from header", 1)
    else:
        integer(boot, "last_seq", rows[0][0])
    if evicted or seq_gaps or queue_drops or int(integrity["overflow"]):
        warnings.append("BLE_EVENT_LOSS_OR_RING_EVICTION")
    if any(int(integrity[k]) for k in ("storage_errors", "unclassified")):
        warnings.append("BLE_NVS_STORAGE_OR_UNKNOWN_FAULT")
    if int(peer_summary["overflow"]):
        warnings.append("PEER_DISCONNECT_SUMMARY_OVERFLOW")
    if faults:
        warnings.append("RECORDED_BLE_OR_CAN_FAULTS")
    if boot["state"] == "RAM_ONLY":
        warnings.append("EVENTS_NOT_CONFIRMED_PERSISTED")
    if not warnings:
        warnings.append("SPARSE_BLE_JOURNAL_ONLY")

    return {
        "kind": "m5can_ble_evidence_sparse_audit",
        "schema": 1,
        "source_sha256": hashlib.sha256(original).hexdigest(),
        "boot": boot_number,
        "firmware_build": boot["build"],
        "firmware_journal_version": version,
        "events_total": total,
        "events_kept": kept,
        "events_evicted": evicted,
        "event_sequence_gaps": seq_gaps,
        "queue_drops": queue_drops,
        "recorded_faults": [
            {"code": int(x["code"]), "count": int(x["count"]),
             "name": x.get("name", "")} for x in faults],
        "recorded_disconnect_peers": len(peers),
        "can_status_histogram": {
            str(x["status"]): int(x["count"]) for x in statuses
        },
        "warnings": warnings,
        "evidence_completeness": {
            "all_ble_events": False,
            "per_query_notification_delivery": False,
            "continuous_can_tx_rx_timeline": False,
            "independent_can_analyzer": False,
        },
        "acceptance": "SPARSE_BLE_JOURNAL_NOT_HARDWARE_APPROVAL",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("blelog_text", type=Path)
    parser.add_argument("output_json", type=Path)
    args = parser.parse_args()
    try:
        require(args.blelog_text.resolve() != args.output_json.resolve(),
                "cannot overwrite source", 1)
        report = audit_blelog(args.blelog_text.read_bytes())
        with args.output_json.open("x", encoding="utf-8") as output:
            json.dump(report, output, sort_keys=True, indent=2)
            output.write("\n")
    except (OSError, EvidenceError, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    print("PASS BLE JOURNAL STRUCTURE (NOT HARDWARE APPROVAL): "
          f"boot={report['boot']} kept={report['events_kept']} "
          f"sha256={report['source_sha256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
