#!/usr/bin/env python3
"""Validate a captured M5CAN Native stream and emit a reproducible acceptance report."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from m5can_native import PacketType, decode_packet, parse_can_frame, parse_stats


_COUNTER_KEYS = ("drop", "missed", "overrun", "buserr", "arb_lost", "native_rx_errors")


def analyze_bytes(blob: bytes, *, min_duration_s: float = 0.0) -> dict[str, Any]:
    report: dict[str, Any] = {
        "packets_seen": 0,
        "decoded_packets": 0,
        "malformed_packets": 0,
        "semantic_errors": 0,
        "sequence_gap_events": 0,
        "missing_packets": 0,
        "sequence_wraps": 0,
        "can_frames": 0,
        "std_frames": 0,
        "ext_frames": 0,
        "rtr_frames": 0,
        "stats_packets": 0,
        "timestamp_regressions": 0,
        "first_timestamp_us": None,
        "last_timestamp_us": None,
        "duration_s": 0.0,
        "frame_rate_hz": 0.0,
        "packet_types": {},
        "max_counters": {key: 0 for key in _COUNTER_KEYS},
        "min_duration_s": float(min_duration_s),
        "acceptance_pass": False,
        "failure_reasons": [],
    }

    previous_sequence: int | None = None
    previous_timestamp: int | None = None

    for encoded in blob.split(b"\x00"):
        if not encoded:
            continue
        report["packets_seen"] += 1
        try:
            packet = decode_packet(encoded)
        except Exception:
            report["malformed_packets"] += 1
            continue

        report["decoded_packets"] += 1
        name = packet.packet_type.name
        report["packet_types"][name] = report["packet_types"].get(name, 0) + 1

        if previous_sequence is not None:
            expected = (previous_sequence + 1) & 0xFFFF
            if packet.sequence != expected:
                report["sequence_gap_events"] += 1
                report["missing_packets"] += (packet.sequence - expected) & 0xFFFF
            elif previous_sequence == 0xFFFF and packet.sequence == 0:
                report["sequence_wraps"] += 1
        previous_sequence = packet.sequence

        if packet.packet_type == PacketType.CAN_FRAME:
            try:
                frame = parse_can_frame(packet)
            except Exception:
                report["semantic_errors"] += 1
                continue

            report["can_frames"] += 1
            report["ext_frames" if frame.extended else "std_frames"] += 1
            if frame.rtr:
                report["rtr_frames"] += 1

            if report["first_timestamp_us"] is None:
                report["first_timestamp_us"] = frame.timestamp_us
            if previous_timestamp is not None and frame.timestamp_us < previous_timestamp:
                report["timestamp_regressions"] += 1
            previous_timestamp = frame.timestamp_us
            report["last_timestamp_us"] = frame.timestamp_us

        elif packet.packet_type == PacketType.STATS:
            try:
                stats = parse_stats(packet)
            except Exception:
                report["semantic_errors"] += 1
                continue
            report["stats_packets"] += 1
            for key in _COUNTER_KEYS:
                report["max_counters"][key] = max(report["max_counters"][key], int(stats[key]))

    first_ts = report["first_timestamp_us"]
    last_ts = report["last_timestamp_us"]
    if first_ts is not None and last_ts is not None and last_ts >= first_ts:
        duration_s = (last_ts - first_ts) / 1_000_000.0
        report["duration_s"] = duration_s
        if duration_s > 0 and report["can_frames"] > 1:
            report["frame_rate_hz"] = (report["can_frames"] - 1) / duration_s

    failures: list[str] = []
    if report["can_frames"] == 0:
        failures.append("no CAN frames")
    if report["stats_packets"] == 0:
        failures.append("no STATS packets")
    if report["malformed_packets"]:
        failures.append(f"malformed_packets={report['malformed_packets']}")
    if report["semantic_errors"]:
        failures.append(f"semantic_errors={report['semantic_errors']}")
    if report["sequence_gap_events"]:
        failures.append(
            f"sequence_gaps={report['sequence_gap_events']} missing={report['missing_packets']}"
        )
    if report["timestamp_regressions"]:
        failures.append(f"timestamp_regressions={report['timestamp_regressions']}")
    if report["duration_s"] < min_duration_s:
        failures.append(
            f"duration_s={report['duration_s']:.3f} below minimum {min_duration_s:.3f}"
        )
    for key, value in report["max_counters"].items():
        if value:
            failures.append(f"{key}={value}")

    report["failure_reasons"] = failures
    report["acceptance_pass"] = not failures
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("--min-duration", type=float, default=0.0,
                        help="required CAN timestamp span in seconds")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()

    report = analyze_bytes(args.input.read_bytes(), min_duration_s=args.min_duration)
    rendered = json.dumps(report, indent=2, sort_keys=True)
    print(rendered)

    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(rendered + "\n", encoding="utf-8")

    return 0 if report["acceptance_pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
