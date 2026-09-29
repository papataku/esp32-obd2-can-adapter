#!/usr/bin/env python3
import random
import struct

from m5can_native import (
    MAX_PAYLOAD, Command, Packet, PacketType, cobs_decode, cobs_encode, crc32,
    decode_packet, encode_command, encode_packet, parse_can_frame, parse_hello,
    parse_stats,
)


def expect_value_error(fn):
    try:
        fn()
    except ValueError:
        return
    raise AssertionError("ValueError was not raised")


def test_cobs():
    cases = [b"", b"abc", b"\0", b"a\0b", bytes(range(256))]
    rng = random.Random(0x5CA1)
    for length in range(0, 301, 7):
        cases.append(bytes(rng.randrange(256) for _ in range(length)))
    for data in cases:
        assert cobs_decode(cobs_encode(data)) == data


def test_cobs_truncated():
    expect_value_error(lambda: cobs_decode(b"\x05abc"))


def test_packet():
    wire = encode_packet(PacketType.EVENT, b"hello\0world", flags=3, sequence=0x1234)
    p = decode_packet(wire[:-1])
    assert p.flags == 3 and p.sequence == 0x1234 and p.payload == b"hello\0world"


def test_crc_corruption_rejected():
    wire = encode_packet(PacketType.EVENT, b"payload", sequence=10)
    raw = bytearray(cobs_decode(wire[:-1]))
    raw[7] ^= 0x01
    damaged = cobs_encode(bytes(raw))
    expect_value_error(lambda: decode_packet(damaged))


def test_unsupported_version_rejected():
    wire = encode_packet(PacketType.EVENT, b"x", sequence=11)
    raw = bytearray(cobs_decode(wire[:-1]))
    raw[0] = 2
    raw[-4:] = struct.pack("<I", crc32(raw[:-4]))
    expect_value_error(lambda: decode_packet(cobs_encode(bytes(raw))))


def test_oversize_payload_rejected():
    expect_value_error(lambda: encode_packet(PacketType.EVENT, b"x" * (MAX_PAYLOAD + 1)))


def test_command():
    p = decode_packet(encode_command(Command.START_STREAM, sequence=7)[:-1])
    assert p.packet_type == PacketType.COMMAND and p.payload == bytes([Command.START_STREAM])


def test_can():
    payload = struct.pack("<QI", 123456, 0x7E8) + bytes([4, 0]) + bytes.fromhex("410C1388")
    f = parse_can_frame(Packet(1, PacketType.CAN_FRAME, 0, 9, payload))
    assert f.identifier == 0x7E8 and f.data == bytes.fromhex("410C1388")


def test_can_dlc_mismatch_rejected():
    payload = struct.pack("<QI", 123456, 0x7E8) + bytes([8, 0]) + bytes.fromhex("410C1388")
    expect_value_error(lambda: parse_can_frame(Packet(1, PacketType.CAN_FRAME, 0, 9, payload)))


def test_rtr():
    payload = struct.pack("<QI", 1, 0x123) + bytes([8, 2])
    f = parse_can_frame(Packet(1, PacketType.CAN_FRAME, 0, 1, payload))
    assert f.rtr and f.dlc == 8 and f.data == b""


def test_hello():
    name, fw = b"M5CAN-Dial", b"0.2.0-phase2"
    payload = bytes([1]) + struct.pack("<I", 500000) + bytes([0, 0, len(name)]) + name + bytes([len(fw)]) + fw
    info = parse_hello(Packet(1, PacketType.HELLO, 0, 1, payload))
    assert info["tx_enabled"] is False and info["stream_enabled"] is False


def test_stats():
    payload = struct.pack("<QQQQQIIIIIBQ", 1,2,3,4,5,6,7,8,9,10,1,11)
    stats = parse_stats(Packet(1, PacketType.STATS, 0, 1, payload))
    assert stats["rx"] == 1 and stats["driver_q"] == 10 and stats["native_rx_errors"] == 11


def test_crc():
    assert crc32(b"123456789") == 0xCBF43926


if __name__ == "__main__":
    tests=[v for k,v in globals().items() if k.startswith("test_") and callable(v)]
    for test in tests:
        test(); print("PASS", test.__name__)
    print(f"{len(tests)} protocol tests passed")
