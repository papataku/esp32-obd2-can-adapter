#!/usr/bin/env python3
import struct

from m5can_native import PacketType, cobs_decode, cobs_encode, crc32, encode_packet
from validate_capture import analyze_bytes


def can_packet(sequence: int, timestamp_us: int, *, extended: bool = False) -> bytes:
    flags = 1 if extended else 0
    payload = (
        struct.pack("<QI", timestamp_us, 0x18DAF110 if extended else 0x7E8)
        + bytes([4, flags])
        + bytes.fromhex("410C1388")
    )
    return encode_packet(PacketType.CAN_FRAME, payload, sequence=sequence)


def stats_packet(sequence: int, *, drop=0, missed=0, overrun=0, buserr=0,
                 arb_lost=0, native_rx_errors=0) -> bytes:
    payload = struct.pack(
        "<QQQQQIIIIIBQ",
        2, 2, 0, 0, drop,
        missed, overrun, buserr, arb_lost, 0,
        1, native_rx_errors,
    )
    return encode_packet(PacketType.STATS, payload, sequence=sequence)


def valid_capture() -> bytes:
    return (
        can_packet(0, 1_000_000)
        + can_packet(1, 2_000_000, extended=True)
        + stats_packet(2)
    )


def test_valid():
    report = analyze_bytes(valid_capture(), min_duration_s=1.0)
    assert report["acceptance_pass"]
    assert report["can_frames"] == 2
    assert report["std_frames"] == 1
    assert report["ext_frames"] == 1
    assert report["duration_s"] == 1.0
    assert report["frame_rate_hz"] == 1.0


def test_sequence_gap():
    blob = can_packet(0, 1_000_000) + can_packet(3, 2_000_000) + stats_packet(4)
    report = analyze_bytes(blob)
    assert not report["acceptance_pass"]
    assert report["sequence_gap_events"] == 1
    assert report["missing_packets"] == 2


def test_crc_corruption():
    wire = bytearray(can_packet(0, 1_000_000))
    raw = bytearray(cobs_decode(bytes(wire[:-1])))
    raw[8] ^= 0x01
    # Do not repair CRC: validator must reject it.
    corrupted = cobs_encode(bytes(raw)) + b"\x00"
    report = analyze_bytes(corrupted + stats_packet(1))
    assert not report["acceptance_pass"]
    assert report["malformed_packets"] == 1


def test_stats_failure():
    blob = can_packet(0, 1_000_000) + can_packet(1, 2_000_000) + stats_packet(2, drop=1)
    report = analyze_bytes(blob)
    assert not report["acceptance_pass"]
    assert report["max_counters"]["drop"] == 1


def test_timestamp_regression():
    blob = can_packet(0, 2_000_000) + can_packet(1, 1_000_000) + stats_packet(2)
    report = analyze_bytes(blob)
    assert not report["acceptance_pass"]
    assert report["timestamp_regressions"] == 1


def test_min_duration():
    report = analyze_bytes(valid_capture(), min_duration_s=2.0)
    assert not report["acceptance_pass"]
    assert any("below minimum" in reason for reason in report["failure_reasons"])


if __name__ == "__main__":
    tests = [v for k, v in globals().items() if k.startswith("test_") and callable(v)]
    for test in tests:
        test()
        print("PASS", test.__name__)
    print(f"{len(tests)} capture validation tests passed")
