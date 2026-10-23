#!/usr/bin/env python3
"""Read-only BLE 574 single-shot evidence audit. Never opens a serial port.

Input is private USB RAW. Output deliberately excludes peer addresses and payload.
A USB line counts only if it has a newline, all mandatory fields and (for V2/
EDGE) a terminal END marker. Cross-device timestamps are NOT assumed aligned.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

PATTERNS = {
    "trace": ("RING_EST_TRACE_V2 ", ("seq", "path", "api", "gap", "disc",
        "mtu_api", "mtu_event", "final", "linked", "dup", "ovf"), True),
    "edge": ("#BLE_EDGE,", ("con", "ok", "err", "last", "disc",
        "reason"), True),
    "edge_task": ("#BLE_EDGE_TASK,", ("interpoll_peak_us",), True),
    "dual": ("#BLE_DUAL,", ("clients", "connects", "disconnects",
        "rx_overflow", "notify", "retries", "failed"), False),
}
INT_FIELD = re.compile(r"^-?[0-9]+$")


def extract(raw: bytes, families):
    rows = {name: [] for name in families}
    invalid = {name: 0 for name in families}
    for index, original in enumerate(raw.splitlines(keepends=True), start=1):
        try:
            line = original.decode("utf-8")
        except UnicodeDecodeError:
            continue
        for name in families:
            prefix, required, require_end = PATTERNS[name]
            if not line.startswith(prefix):
                continue
            if not original.endswith(b"\n"):
                invalid[name] += 1
                break
            text = line.rstrip("\r\n")
            if require_end:
                if not text.endswith(" END"):
                    invalid[name] += 1
                    break
                text = text[:-4]
            payload = text[len(prefix):].replace(",", " ")
            items = payload.split()
            parsed = {}
            for item in items:
                if "=" in item:
                    k, v = item.split("=", 1)
                    if k not in parsed:
                        parsed[k] = v
            if any(k not in parsed for k in required):
                invalid[name] += 1
                break
            numeric = [k for k in required if k != "path"]
            if any(not INT_FIELD.fullmatch(parsed[k]) for k in numeric):
                invalid[name] += 1
                break
            # Keep the validated numeric values only; never reveal addresses.
            entry = {"source_line": index}
            entry.update({k: (parsed[k] if k == "path" else int(parsed[k]))
                          for k in required})
            # Optional timing is always local to its source device.
            for extra in ("gap_dt", "disc_dt", "mtu_dt", "last_ms",
                          "first_ms", "disc_ms", "first"):
                if extra in parsed and INT_FIELD.fullmatch(parsed[extra]):
                    entry[extra] = int(parsed[extra])
            rows[name].append(entry)
            break
    return rows, invalid


def endpoints(rows, keys):
    if not rows:
        return None
    return {"samples": len(rows),
            "first": {k: rows[0][k] for k in keys},
            "last": {k: rows[-1][k] for k in keys},
            "first_line": rows[0]["source_line"],
            "last_line": rows[-1]["source_line"]}


def analyze(ring: bytes, m5: bytes):
    rr, ri = extract(ring, ("trace",))
    mr, mi = extract(m5, ("edge", "edge_task", "dual"))
    trace, edge, dual = rr["trace"], mr["edge"], mr["dual"]
    warnings = ["No cross-device timestamp synchronization; do not infer RF cause.",
                "M5 notify counters measure queued notifications, not peer ACKs.",
                "App connection state and GAP observation are distinct."]
    anomalies = []
    if len(trace) > 1:
        anomalies.append("more_than_one_ring_trace_in_capture")
    if any(r["clients"] > 2 or r["clients"] < 0 for r in dual):
        anomalies.append("m5_peer_limit_exceeded")
    if any(r["dup"] > 0 or r["ovf"] > 0 for r in trace):
        anomalies.append("ring_trace_duplicate_or_overflow")
    if any(x["err"] > x["con"] or x["ok"] > x["con"] or
           x["ok"] + x["err"] > x["con"] for x in edge):
        anomalies.append("invalid_m5_gap_counters")
    for name, data, keys in (
        ("m5_edge", edge, ("con", "ok", "err", "disc")),
        ("m5_dual", dual, ("connects", "disconnects", "notify"))):
        for left, right in zip(data, data[1:]):
            if any(right[k] < left[k] for k in keys):
                anomalies.append(name + "_counter_regression")
                break

    result = "INCOMPLETE_EVIDENCE"
    last = trace[0] if len(trace) == 1 else None
    if last is not None:
        if last["final"] != 0 or last["linked"] == 0:
            result = "FAIL_STOP"
        elif (last["path"] == "SUCCESS" and last["api"] == 0 and
              last["gap"] == 0 and last["mtu_api"] == 0 and
              last["mtu_event"] >= 23 and last["final"] == 0 and
              last["linked"] == 1):
            # This means one successful Ring trace, NOT a product PASS.
            result = "RING_SINGLE_CONNECT_SUCCESS_ONLY"
        else:
            warnings.append("Ring final=0 without complete required V2 success fields.")
    else:
        warnings.append("No unique complete RING_EST_TRACE_V2 line.")
    if not edge:
        warnings.append("No complete M5 #BLE_EDGE; peripheral GAP unobserved.")
    if not dual:
        warnings.append("No complete M5 #BLE_DUAL; application connection unobserved.")
    if any(ri.values()) or any(mi.values()):
        warnings.append("Incomplete/invalid device lines excluded from classification.")
    if anomalies:
        result = "STOP_ANOMALY"
    return {
        "classification": result,
        "ring": {
            "sha256": hashlib.sha256(ring).hexdigest(),
            "complete_v2": trace,
            "incomplete_v2": ri["trace"],
        },
        "m5": {
            "sha256": hashlib.sha256(m5).hexdigest(),
            "gap": endpoints(edge, ("con", "ok", "err", "disc")),
            "dual": endpoints(dual, ("clients", "connects", "disconnects",
                                   "rx_overflow", "notify", "retries", "failed")),
            "interpoll_peak_us": max((row["interpoll_peak_us"] for row in
                                       mr["edge_task"]), default=None),
            "complete_edge_task_samples": len(mr["edge_task"]),
            "incomplete": mi,
        },
        "anomalies": sorted(set(anomalies)),
        "warnings": warnings,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ring", type=Path, required=True)
    parser.add_argument("--m5", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = analyze(args.ring.read_bytes(), args.m5.read_bytes())
    formatted = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(formatted, encoding="utf-8")
    else:
        print(formatted, end="")
    if result["classification"] == "STOP_ANOMALY":
        raise SystemExit(2)


if __name__ == "__main__":
    main()
