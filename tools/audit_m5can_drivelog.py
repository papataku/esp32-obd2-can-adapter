#!/usr/bin/env python3
"""Read-only audit of the firmware #DRIVELOG NVS/USB text export.

This is a *sparse journal*, not a per-request CAN/BLE trace. The audit
deliberately cannot produce the hardware-provenance event JSONL accepted by
validate_multi_ecu_bench_trace.py. It reports factual counters and gaps only.

Requires an actual complete #DRIVELOG,BEGIN ... #DRIVELOG,END dump.
No serial/BLE/CAN access; no ability to change firmware or vehicle state.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

from validate_multi_ecu_bench_trace import EvidenceError

KNOWN = {"CAN", "TIMING", "MEMORY", "COORD", "PROBE", "PROBE_RAW",
         "GROUPFAIL", "GROUPFAIL_RAW", "EVENT", "RAW"}


def ensure(check, why, line):
    if not check:
        raise EvidenceError(f"drive log line {line}: {why}")


def values(items, line):
    result = {}
    for item in items:
        ensure("=" in item, "invalid key=value item", line)
        key, value = item.split("=", 1)
        ensure(key and (value or key == "data") and key not in result,
               "empty/duplicate drive log attribute", line)
        result[key] = value
    return result


def decimal(d, key, line):
    ensure(key in d and d[key].isdigit(),
           f"missing/invalid decimal {key}", line)
    return int(d[key])


def hexid(d, key, line):
    text = d.get(key, "")
    ensure(1 <= len(text) <= 8 and
           all(c in "0123456789ABCDEF" for c in text),
           f"invalid hex {key}", line)
    return int(text, 16)


def audit_drivelog(original: bytes) -> dict:
    try:
        lines = original.decode("utf-8").splitlines()
    except UnicodeError as exc:
        raise EvidenceError("DRIVELOG is not UTF-8") from exc
    entries = [(no, ln.strip()) for no, ln in enumerate(lines, 1) if ln.strip()]
    ensure(bool(entries), "empty #DRIVELOG dump", 1)
    ensure(entries[0][1].startswith("#DRIVELOG,BEGIN,version=2,"),
           "requires version 2 BEGIN marker", entries[0][0])
    ensure(entries[-1][1] == "#DRIVELOG,END",
           "truncated: missing END marker", entries[-1][0])
    ensure(not any(not ln.startswith("#DRIVELOG,") for _, ln in entries),
           "unexpected non-DRIVELOG line in dump", 1)
    ensure(len(entries) >= 5, "empty/incomplete journal", 1)
    boots = {}
    current = None
    for num, row in entries[1:-1]:
        fields = row.split(",")
        kind = fields[1]
        if kind.startswith("boot="):
            summary = values(fields[1:], num)
            boot_id = decimal(summary, "boot", num)
            ensure(boot_id not in boots, "duplicate boot record", num)
            for key in ("uptime_ms", "saves", "grouped_submissions",
                        "individual_submissions", "clients",
                        "disconnects", "fault"):
                decimal(summary, key, num)
            ensure(summary.get("build"), "missing firmware build", num)
            current = {
                "boot": boot_id,
                "build": summary["build"],
                "summary": summary,
                "can": None,
                "timing": None,
                "memory": None,
                "coord": None,
                "probes": {},
                "probe_raw": {"PROBE": [], "GROUPFAIL": []},
                "events": [],
                "raw": [],
                "warnings": [],
            }
            boots[boot_id] = current
            continue
        ensure(kind in KNOWN, f"unknown journal entry {kind}", num)
        ensure(current is not None, f"{kind} before boot record", num)
        v = values(fields[2:], num)
        ensure(decimal(v, "boot", num) == current["boot"],
               f"{kind} associated with wrong boot", num)

        if kind in {"CAN", "TIMING", "MEMORY", "COORD"}:
            ensure(current[kind.lower()] is None,
                   f"duplicate {kind} entry", num)
            current[kind.lower()] = v
            if kind == "CAN":
                tx = v.get("tx", "").split("/")
                ensure(len(tx) == 2 and all(x.isdigit() for x in tx),
                       "invalid tx=successful/attempts count", num)
                ensure(int(tx[0]) <= int(tx[1]), "successful TX > attempts", num)
                for key in ("rx", "q", "ok", "no_data", "failed",
                            "timeout", "drop", "buserr", "missed", "overrun",
                            "BLE_rx_overflow", "notify_failed", "retries"):
                    decimal(v, key, num)
                v["tx_success"] = int(tx[0])
                v["tx_attempts"] = int(tx[1])
        elif kind in {"PROBE", "GROUPFAIL"}:
            ensure(kind not in current["probes"], f"duplicate {kind}", num)
            current["probes"][kind] = v
            if v.get("state") != "MISSING":
                for key in ("raw_saved", "raw_count", "truncated", "responses",
                            "status", "transaction_us", "first_response_us"):
                    decimal(v, key, num)
                ensure(int(v["raw_saved"]) <= int(v["raw_count"]),
                       f"{kind} saved frame count exceeds actual", num)
                ensure(v.get("origin") in {"CAN", "SYNTHETIC"},
                       f"{kind} frame origin invalid", num)
        elif kind in {"PROBE_RAW", "GROUPFAIL_RAW"}:
            parent = "PROBE" if kind == "PROBE_RAW" else "GROUPFAIL"
            ensure(parent in current["probes"] and
                   current["probes"][parent].get("state") != "MISSING",
                   f"{kind} without corresponding {parent}", num)
            count = decimal(v, "dlc", num)
            ensure(count <= 8, f"{kind} invalid DLC", num)
            hexid(v, "id", num)
            payload = v.get("data", "")
            ensure(len(payload) == 2*count and
                   all(c in "0123456789ABCDEF" for c in payload),
                   f"{kind} bad raw CAN bytes", num)
            decimal(v, "index", num)
            decimal(v, "rx_tx_us", num)
            current["probe_raw"][parent].append(v)
        elif kind == "EVENT":
            for key in ("ms", "kind", "peer", "status",
                        "responses", "raw_count", "transaction_us"):
                decimal(v, key, num)
            hexid(v, "ecu", num)
            ensure(len(current["events"]) < 8,
                   "journal contains more than eight state events", num)
            current["events"].append(v)
        elif kind == "RAW":
            event_ix = decimal(v, "event", num)
            ensure(event_ix < len(current["events"]) and
                   event_ix == len(current["events"]) - 1,
                   "RAW frame not associated with latest journal event", num)
            count = decimal(v, "dlc", num)
            ensure(count <= 8, "RAW invalid DLC", num)
            ensure(len(v.get("data", "")) == count*2 and
                   all(c in "0123456789ABCDEF" for c in v.get("data", "")),
                   "RAW corrupt CAN payload", num)
            hexid(v, "id", num)
            current["raw"].append(v)
    ensure(bool(boots), "dump has no boot record", 1)
    warnings = []
    report = []
    for boot, current in sorted(boots.items()):
        ensure(current["can"] is not None and
               current["timing"] is not None and
               current["memory"] is not None,
               f"boot {boot} lacks mandatory CAN/TIMING/MEMORY", 1)
        for name, probe in current["probes"].items():
            raw = current["probe_raw"][name]
            if probe.get("state") == "MISSING":
                ensure(not raw, f"boot {boot} has RAW for missing probe", 1)
                warnings.append(f"boot={boot}:{name}:MISSING")
                continue
            saved = int(probe["raw_saved"])
            ensure(len(raw) == saved,
                   f"boot {boot} {name}: actual RAW count != raw_saved", 1)
            ensure(sorted(int(x["index"]) for x in raw) == list(range(saved)),
                   f"boot {boot} {name}: RAW frame indices missing/duplicated", 1)
            if probe["origin"] == "SYNTHETIC":
                warnings.append(f"boot={boot}:{name}:SYNTHETIC")
            if int(probe["truncated"]) or saved < int(probe["raw_count"]):
                warnings.append(f"boot={boot}:{name}:TRUNCATED")
        for event_ix, ev in enumerate(current["events"]):
            raw_count = sum(int(x["event"]) == event_ix for x in current["raw"])
            ensure(raw_count == min(2, int(ev["raw_count"])),
                   f"boot {boot} event {event_ix}: RAW count does not match event", 1)
        can = current["can"]
        drops = {k: int(can[k]) for k in
                 ("drop", "buserr", "missed", "overrun",
                  "BLE_rx_overflow", "notify_failed") if int(can[k])}
        if drops:
            warnings.append(f"boot={boot}:ERROR_COUNTERS:{drops}")
        # The NVS journal contains at most eight latest state/fault events;
        # it never claims to hold every request, generation or TX frame.
        report.append({
            "boot": boot,
            "firmware_build": current["build"],
            "recorded_uptime_ms": int(current["summary"]["uptime_ms"]),
            "query_count": int(can["q"]),
            "can_rx_total": int(can["rx"]),
            "can_tx_attempts": can["tx_attempts"],
            "can_tx_success": can["tx_success"],
            "recorded_state_events": len(current["events"]),
            "recorded_raw_event_frames": len(current["raw"]),
            "recorded_probe_kinds": sorted(current["probes"]),
            "error_counters": drops,
            "firmware_fault_flag": int(current["summary"]["fault"]),
        })
    if not warnings:
        warnings.append("SPARSE_JOURNAL_ONLY")
    return {
        "kind": "m5can_drivelog_sparse_audit",
        "schema": 1,
        "source_sha256": hashlib.sha256(original).hexdigest(),
        "firmware_journal_version": 2,
        "boot_count": len(report),
        "boots": report,
        "warnings": warnings,
        "evidence_completeness": {
            "full_can_tx_rx_history": False,
            "all_ble_connection_generations": False,
            "per_query_ble_delivery_provenance": False,
            "independent_can_analyzer": False,
            "physical_multiecu_tx_validation": False,
        },
        "acceptance": "PARTIAL_JOURNAL_ONLY_NOT_HARDWARE_APPROVAL",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("drivelog_text", type=Path)
    parser.add_argument("report_json", type=Path)
    args = parser.parse_args()
    try:
        ensure(args.drivelog_text.resolve() != args.report_json.resolve(),
               "output may not overwrite source", 1)
        result = audit_drivelog(args.drivelog_text.read_bytes())
        with args.report_json.open("x", encoding="utf-8") as file:
            json.dump(result, file, sort_keys=True, indent=2)
            file.write("\n")
    except (OSError, EvidenceError, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    print("PASS SPARSE LOG STRUCTURE (NOT HARDWARE APPROVAL): "
          f"boots={result['boot_count']} "
          f"warnings={len(result['warnings'])} "
          f"sha256={result['source_sha256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
