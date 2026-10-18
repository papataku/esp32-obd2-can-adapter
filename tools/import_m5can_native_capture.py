#!/usr/bin/env python3
"""Strict offline import of an existing M5CAN Native COBS capture.

This imports *device-observed receive traffic only*. It cannot prove that
M5CAN did or did not transmit frames, or that a BLE reply went to a certain
peer. Those claims require the independent CAN analyzer and per-session BLE
evidence described by MULTI_ECU_BENCH_EVIDENCE_SCHEMA.md.

No CAN interface, BLE device, serial port or network is accessed. The output
is deliberately NOT the 'capture_meta' type accepted for an independent CAN
analyzer by reconcile_multi_ecu_can_capture.py.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

from m5can_native import (PacketType, decode_packet, parse_can_frame,
                          parse_hello, parse_stats)
from validate_multi_ecu_bench_trace import EvidenceError

ERROR_COUNTERS = ("drop", "missed", "overrun", "buserr",
                  "arb_lost", "native_rx_errors")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def import_native_capture(blob: bytes) -> bytes:
    """Import and strictly validate a complete Native binary COBS stream.

    Reject partial tails, all decoder/CRC/sequence errors and unknown stats.
    The source SHA-256 commits to the *unmodified original input bytes*.
    """
    require(blob and blob.endswith(b"\x00"),
            "Native capture is empty or lacks final COBS packet delimiter")
    segments = blob[:-1].split(b"\x00")
    require(all(segments), "Native capture contains empty/missing packets")

    hello = None
    last_sequence = None
    last_timestamp = -1
    last_stats = None
    can_rows: list[dict] = []
    packet_count = 0
    stats_count = 0

    for index, encoded in enumerate(segments, 1):
        try:
            packet = decode_packet(encoded)
        except (ValueError, KeyError) as exc:
            raise EvidenceError(f"Native packet {index}: invalid COBS/CRC: {exc}") from exc
        if last_sequence is not None:
            require(packet.sequence == (last_sequence + 1) & 0xFFFF,
                    f"Native packet {index}: sequence gap or packet reorder")
        last_sequence = packet.sequence
        packet_count += 1

        if packet.packet_type == PacketType.HELLO:
            require(hello is None, "Native capture has duplicate HELLO/boot segment")
            try:
                hello = parse_hello(packet)
            except ValueError as exc:
                raise EvidenceError(f"Native HELLO invalid: {exc}") from exc
            require(hello["protocol"] == 1, "unsupported Native protocol version")
            require(hello["bitrate"] in (125000, 250000, 500000, 1000000),
                    "Native HELLO bitrate is unrecognized")
            require(bool(hello["device"]) and bool(hello["firmware"]),
                    "Native HELLO missing device or firmware identity")
        elif packet.packet_type == PacketType.CAN_FRAME:
            try:
                frame = parse_can_frame(packet)
            except ValueError as exc:
                raise EvidenceError(f"Native CAN packet {index}: {exc}") from exc
            require(frame.timestamp_us >= last_timestamp,
                    f"Native CAN packet {index}: timestamp regressed")
            require(frame.identifier <= (0x1FFFFFFF if frame.extended else 0x7FF),
                    f"Native CAN packet {index}: arbitration ID out of range")
            last_timestamp = frame.timestamp_us
            can_rows.append({
                "t_us": frame.timestamp_us,
                "can_id": f"{frame.identifier:08X}",
                "data_hex": frame.data.hex().upper(),
                "dlc": frame.dlc,
                "extended": frame.extended,
                "rtr": frame.rtr,
                "self_reception": frame.self_reception,
                "native_packet_sequence": packet.sequence,
            })
        elif packet.packet_type == PacketType.STATS:
            try:
                stats = parse_stats(packet)
            except ValueError as exc:
                raise EvidenceError(f"Native STATS packet {index}: {exc}") from exc
            for key in ERROR_COUNTERS:
                require(stats[key] == 0,
                        f"Native STATS {key}={stats[key]}: incomplete/errored capture")
            if last_stats is not None:
                for key in ("rx", "std", "ext", "rtr"):
                    require(stats[key] >= last_stats[key],
                            f"Native STATS counter {key} regressed")
            last_stats = stats
            stats_count += 1
        elif packet.packet_type in (PacketType.ERROR, PacketType.COMMAND):
            raise EvidenceError(f"Native packet {index}: unexpected error/host command")
        # Event and command-reply packets are metadata, not frame observations.

    require(hello is not None, "Native capture lacks HELLO identity")
    require(can_rows, "Native capture lacks CAN frames")
    require(stats_count > 0, "Native capture lacks STATS/loss counters")
    require(last_stats is not None, "Native STATS missing")
    require(last_stats["rx"] >= len(can_rows),
            "Native reported RX count is lower than observed CAN packets")
    require(last_stats["std"] + last_stats["ext"] >= len(can_rows),
            "Native reported frame-class totals lower than CAN packets")

    meta = {
        "type": "device_capture_meta",
        "schema": 1,
        "origin": "m5can_native_rx",
        "independent_analyzer": False,
        "source_sha256": hashlib.sha256(blob).hexdigest(),
        "packet_count": packet_count,
        "can_frame_count": len(can_rows),
        "stats_count": stats_count,
        "last_native_sequence": last_sequence,
        "timestamp_origin": "device_native_monotonic_us",
        "device": hello["device"],
        "firmware": hello["firmware"],
        "protocol": hello["protocol"],
        "bitrate": hello["bitrate"],
        "hello_tx_enabled": hello["tx_enabled"],
        "hello_stream_enabled": hello["stream_enabled"],
        "last_stats": last_stats,
    }
    rows = [meta] + can_rows
    return ("\n".join(json.dumps(row, sort_keys=True, separators=(",", ":"))
                      for row in rows) + "\n").encode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("native_capture", type=Path)
    parser.add_argument("output_jsonl", type=Path)
    args = parser.parse_args()
    try:
        require(args.native_capture.resolve() != args.output_jsonl.resolve(),
                "source and output file must differ")
        blob = args.native_capture.read_bytes()
        result = import_native_capture(blob)
        # An existing evidence file cannot be silently overwritten.
        with args.output_jsonl.open("xb") as output:
            output.write(result)
    except (EvidenceError, OSError, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    count = len(result.splitlines()) - 1
    print("PASS DEVICE RX IMPORT (NOT INDEPENDENT HARDWARE EVIDENCE): "
          f"input_sha256={hashlib.sha256(blob).hexdigest()} "
          f"output_sha256={hashlib.sha256(result).hexdigest()} "
          f"rx_frames={count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
