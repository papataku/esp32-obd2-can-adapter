#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path

from m5can_native import PacketType, decode_packet, parse_can_frame, parse_hello, parse_stats


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("input", type=Path)
    ap.add_argument("--jsonl", type=Path)
    args = ap.parse_args()

    blob = args.input.read_bytes()
    good = bad = 0
    out = args.jsonl.open("w", encoding="utf-8") if args.jsonl else None
    try:
        for encoded in blob.split(b"\x00"):
            if not encoded:
                continue
            try:
                p = decode_packet(encoded)
                good += 1
                record = {"seq": p.sequence, "type": p.packet_type.name}
                if p.packet_type == PacketType.CAN_FRAME:
                    f = parse_can_frame(p)
                    record.update(timestamp_us=f.timestamp_us, id=f.identifier,
                                  extended=f.extended, rtr=f.rtr, dlc=f.dlc,
                                  data=f.data.hex().upper())
                elif p.packet_type == PacketType.HELLO:
                    record.update(parse_hello(p))
                elif p.packet_type == PacketType.STATS:
                    record.update(parse_stats(p))
                elif p.packet_type == PacketType.EVENT:
                    record["message"] = p.payload.decode("utf-8", "replace")
                if out:
                    out.write(json.dumps(record, ensure_ascii=False) + "\n")
            except Exception as exc:
                bad += 1
                print(f"bad packet #{good + bad}: {exc}")
    finally:
        if out:
            out.close()
    print(f"good={good} bad={bad}")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
