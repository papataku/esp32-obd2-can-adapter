#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
import time

import serial
import serial.tools.list_ports

from m5can_native import Command, PacketType, decode_packet, encode_command, parse_can_frame, parse_hello, parse_stats


def wait_text_marker(dev: serial.Serial, command: bytes, marker: bytes, timeout: float = 8.0) -> None:
    deadline = time.monotonic() + timeout
    next_send = 0.0
    while time.monotonic() < deadline:
        now = time.monotonic()
        if now >= next_send:
            dev.write(command + b"\r\n")
            next_send = now + 0.5
        line = dev.readline()
        if line and marker in line:
            return
    raise TimeoutError(f"no marker {marker!r} for {command!r}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", type=Path)
    ap.add_argument("--jsonl", type=Path)
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    if args.list:
        for p in serial.tools.list_ports.comports():
            print(f"{p.device}\t{p.description}")
        return 0
    if not args.port:
        ap.error("--port is required")

    stamp = time.strftime("%Y%m%d-%H%M%S")
    out = args.out or Path("logs") / f"m5can-{stamp}.m5can"
    out.parent.mkdir(parents=True, exist_ok=True)
    if args.jsonl:
        args.jsonl.parent.mkdir(parents=True, exist_ok=True)

    host_seq = 1
    last_device_seq = None
    missing = malformed = frames = 0
    last_report_frames = 0
    buffer = bytearray()
    json_fp = args.jsonl.open("w", encoding="utf-8", buffering=1) if args.jsonl else None

    try:
        with serial.Serial(args.port, args.baud, timeout=0.1) as dev, out.open("wb", buffering=0) as raw:
            dev.reset_input_buffer()
            wait_text_marker(dev, b"STREAM OFF", b"#OK,STREAM,OFF")
            wait_text_marker(dev, b"NATIVE ON", b"#OK,NATIVE,ON")
            dev.write(encode_command(Command.START_STREAM, sequence=host_seq))
            host_seq = (host_seq + 1) & 0xFFFF

            print(f"capturing {args.port} -> {out}")
            while True:
                chunk = dev.read(4096)
                if chunk:
                    buffer.extend(chunk)
                while True:
                    try:
                        end = buffer.index(0)
                    except ValueError:
                        break
                    encoded = bytes(buffer[:end])
                    del buffer[:end + 1]
                    if not encoded:
                        continue
                    raw.write(encoded + b"\x00")
                    try:
                        packet = decode_packet(encoded)
                    except Exception as exc:
                        malformed += 1
                        print(f"bad packet: {exc}")
                        continue

                    if last_device_seq is not None:
                        expected = (last_device_seq + 1) & 0xFFFF
                        if packet.sequence != expected:
                            gap = (packet.sequence - expected) & 0xFFFF
                            missing += gap
                            print(f"sequence gap expected={expected} got={packet.sequence} missing={gap}")
                    last_device_seq = packet.sequence

                    record = {"seq": packet.sequence, "type": packet.packet_type.name}
                    if packet.packet_type == PacketType.CAN_FRAME:
                        f = parse_can_frame(packet)
                        frames += 1
                        record.update(timestamp_us=f.timestamp_us, id=f.identifier,
                                      extended=f.extended, rtr=f.rtr, dlc=f.dlc,
                                      data=f.data.hex().upper())
                    elif packet.packet_type == PacketType.HELLO:
                        record.update(parse_hello(packet))
                        print("HELLO", json.dumps(record, ensure_ascii=False))
                    elif packet.packet_type == PacketType.STATS:
                        record.update(parse_stats(packet))
                    elif packet.packet_type == PacketType.EVENT:
                        record["message"] = packet.payload.decode("utf-8", "replace")

                    if json_fp:
                        json_fp.write(json.dumps(record, ensure_ascii=False) + "\n")

                if frames - last_report_frames >= 5000:
                    last_report_frames = frames
                    print(f"frames={frames} malformed={malformed} missing={missing}")
    except KeyboardInterrupt:
        pass
    finally:
        if json_fp:
            json_fp.close()

    print(f"stopped frames={frames} malformed={malformed} missing={missing} file={out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
