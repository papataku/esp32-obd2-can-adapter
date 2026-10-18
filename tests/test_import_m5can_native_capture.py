#!/usr/bin/env python3
"""Entire suite uses synthetic Native packets, NOT real CAN hardware captures."""
import hashlib
import json
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from m5can_native import (PacketType, cobs_decode, cobs_encode, encode_packet)
from import_m5can_native_capture import import_native_capture, EvidenceError
from reconcile_multi_ecu_can_capture import parse_raw_capture


def hello(seq=0, *, tx=False, bitrate=500000):
    device, version = b"M5CAN", b"0.4.4"
    payload = (bytes((1,)) + struct.pack("<I", bitrate) +
               bytes((int(tx), 1, len(device))) + device +
               bytes((len(version),)) + version)
    return encode_packet(PacketType.HELLO, payload, sequence=seq)


def frame(seq, t, can_id, data=b"", *, extended=True,
          rtr=False, dlc=None, self_reception=False):
    if dlc is None:
        dlc = len(data)
    flags = int(extended) | (int(rtr) << 1) | (int(self_reception) << 2)
    payload = struct.pack("<QI", t, can_id) + bytes((dlc, flags))
    if not rtr:
        payload += data
    return encode_packet(PacketType.CAN_FRAME, payload, sequence=seq)


def stats(seq, *, rx=2, std=1, ext=1, rtr=0,
          drop=0, missed=0, overrun=0, buserr=0,
          arb_lost=0, native_rx_errors=0):
    payload = struct.pack(
        "<QQQQQIIIIIBQ", rx, std, ext, rtr, drop,
        missed, overrun, buserr, arb_lost, 0, 1, native_rx_errors)
    return encode_packet(PacketType.STATS, payload, sequence=seq)


def example():
    return (hello() +
            frame(1, 100, 0x18DAF102, bytes.fromhex("04622012AA"),
                  extended=True) +
            frame(2, 120, 0x7E8, bytes.fromhex("04410C1388"),
                  extended=False) +
            stats(3))


class NativeImportTests(unittest.TestCase):
    def must_fail(self, wire, pattern):
        with self.assertRaisesRegex(EvidenceError, pattern):
            import_native_capture(wire)

    def test_pass_preserves_bytes_and_marks_non_independent(self):
        raw = example()
        jsonl = import_native_capture(raw)
        lines = [json.loads(x) for x in jsonl.splitlines()]
        meta = lines[0]
        self.assertEqual(meta["type"], "device_capture_meta")
        self.assertFalse(meta["independent_analyzer"])
        self.assertEqual(meta["origin"], "m5can_native_rx")
        self.assertEqual(meta["source_sha256"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(meta["last_stats"]["drop"], 0)
        self.assertEqual(meta["can_frame_count"], 2)
        self.assertEqual(lines[1]["t_us"], 100)
        self.assertEqual(lines[1]["can_id"], "18DAF102")
        self.assertEqual(lines[1]["data_hex"], "04622012AA")
        self.assertTrue(lines[1]["extended"])
        self.assertEqual(lines[2]["can_id"], "000007E8")
        self.assertFalse(lines[2]["extended"])
        with self.assertRaisesRegex(EvidenceError, "metadata missing"):
            parse_raw_capture(jsonl)

    def test_native_can_reception_cannot_prove_no_device_tx(self):
        raw = hello(tx=True) + frame(1, 100, 0x18DAF102,
                                     bytes.fromhex("04622012AA")) + stats(2, rx=1, std=0, ext=1)
        meta = json.loads(import_native_capture(raw).splitlines()[0])
        self.assertTrue(meta["hello_tx_enabled"])
        self.assertFalse(meta["independent_analyzer"])

    def test_remote_and_self_reception_flags_preserved(self):
        raw = hello() + frame(1, 100, 0x7E8, extended=False,
                              rtr=True, dlc=8, self_reception=True) + stats(2, rx=1, std=1, ext=0, rtr=1)
        event = json.loads(import_native_capture(raw).splitlines()[1])
        self.assertTrue(event["rtr"])
        self.assertTrue(event["self_reception"])
        self.assertEqual(event["dlc"], 8)
        self.assertEqual(event["data_hex"], "")

    def test_crc_corruption_is_rejected(self):
        wire = example()
        segments = wire.split(b"\x00")
        corrupted = bytearray(cobs_decode(segments[1]))
        corrupted[8] ^= 1
        segments[1] = cobs_encode(bytes(corrupted))
        self.must_fail(b"\x00".join(segments), "invalid COBS/CRC")

    def test_gap_reorder_duplicate_and_missing_terminator(self):
        self.must_fail(hello() + frame(2, 100, 0x18DAF102, b"abc") +
                       stats(3, rx=1, std=0, ext=1), "sequence gap")
        self.must_fail(example()[:-1], "lacks final COBS")
        self.must_fail(hello() + b"\x00" + frame(1, 100, 0x18DAF102, b"abc") +
                       stats(2, rx=1, std=0, ext=1), "empty/missing packets")
        self.must_fail(hello() + hello(1) + frame(2, 100, 0x18DAF102, b"abc") +
                       stats(3, rx=1, std=0, ext=1), "duplicate HELLO")

    def test_fail_on_missing_identity_counters_frames(self):
        self.must_fail(frame(0, 100, 0x7E8, b"abc", extended=False) +
                       stats(1, rx=1, std=1, ext=0), "lacks HELLO")
        self.must_fail(hello() + frame(1, 100, 0x7E8, b"abc", extended=False),
                       "lacks STATS")
        self.must_fail(hello() + stats(1, rx=0, std=0, ext=0),
                       "lacks CAN frames")
        self.must_fail(hello() + frame(1, 100, 0x7E8, b"abc", extended=False) +
                       stats(2, rx=0, std=0, ext=0), "reported RX count")
        self.must_fail(hello() + frame(1, 100, 0x7E8, b"abc", extended=False) +
                       stats(2, rx=1, std=0, ext=0), "frame-class totals")

    def test_stats_losses_are_hard_failures(self):
        for counter in ["drop", "missed", "overrun", "buserr", "arb_lost",
                        "native_rx_errors"]:
            kwargs = {counter: 1}
            wire = hello() + frame(1, 100, 0x7E8, b"abc", extended=False) + stats(2, rx=1, std=1, ext=0, **kwargs)
            self.must_fail(wire, f"STATS {counter}=1")

    def test_timestamp_regression_and_id_range(self):
        raw = hello() + frame(1, 120, 0x7E8, b"abc", extended=False) + frame(2, 100, 0x7E8, b"def", extended=False) + stats(3, rx=2, std=2, ext=0)
        self.must_fail(raw, "timestamp regressed")
        raw = hello() + frame(1, 100, 0x1000, b"abc", extended=False) + stats(2, rx=1, std=1, ext=0)
        self.must_fail(raw, "arbitration ID out of range")

    def test_truncated_frame_invalid_stats_and_bad_hello(self):
        raw = hello() + frame(1, 100, 0x18DAF102, b"abc", dlc=7) + stats(2, rx=1, std=0, ext=1)
        self.must_fail(raw, "bad CAN frame length")
        raw = hello(bitrate=42) + frame(1, 100, 0x7E8, b"abc", extended=False) + stats(2, rx=1, std=1, ext=0)
        self.must_fail(raw, "bitrate is unrecognized")
        raw = hello() + frame(1, 100, 0x7E8, b"abc", extended=False) + encode_packet(PacketType.STATS, b"bad", sequence=2)
        self.must_fail(raw, "STATS packet")

    def test_sequence_wraparound(self):
        raw = (hello(seq=65534) +
               frame(65535, 100, 0x7E8, b"abc", extended=False) +
               stats(0, rx=1, std=1, ext=0))
        self.assertEqual(json.loads(import_native_capture(raw).splitlines()[0])["last_native_sequence"], 0)

    def test_error_packet_is_rejected(self):
        wire = hello() + frame(1, 100, 0x7E8, b"abc", extended=False) + encode_packet(PacketType.ERROR, b"x", sequence=2) + stats(3, rx=1, std=1, ext=0)
        self.must_fail(wire, "unexpected error/host command")


if __name__ == "__main__":
    unittest.main()
