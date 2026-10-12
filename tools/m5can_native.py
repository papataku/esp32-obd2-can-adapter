#!/usr/bin/env python3
from __future__ import annotations

import binascii
import dataclasses
import enum
import struct

VERSION = 1
MAX_PAYLOAD = 128
_HEADER = struct.Struct("<BBBHH")
_CRC = struct.Struct("<I")


class PacketType(enum.IntEnum):
    HELLO = 0x01
    STATS = 0x02
    CAN_FRAME = 0x03
    EVENT = 0x04
    ERROR = 0x05
    COMMAND = 0x10
    COMMAND_REPLY = 0x11


class Command(enum.IntEnum):
    PING = 0x01
    GET_INFO = 0x02
    GET_STATS = 0x03
    START_STREAM = 0x04
    STOP_STREAM = 0x05
    TEXT_MODE = 0x06


@dataclasses.dataclass(frozen=True)
class Packet:
    version: int
    packet_type: PacketType
    flags: int
    sequence: int
    payload: bytes


@dataclasses.dataclass(frozen=True)
class CanFrame:
    timestamp_us: int
    identifier: int
    dlc: int
    extended: bool
    rtr: bool
    self_reception: bool
    data: bytes


def crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def cobs_encode(data: bytes) -> bytes:
    out = bytearray(b"\x00")
    code_index = 0
    code = 1
    for value in data:
        if value == 0:
            out[code_index] = code
            code_index = len(out)
            out.append(0)
            code = 1
        else:
            out.append(value)
            code += 1
            if code == 0xFF:
                out[code_index] = code
                code_index = len(out)
                out.append(0)
                code = 1
    out[code_index] = code
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    if not data:
        raise ValueError("empty COBS packet")
    out = bytearray()
    pos = 0
    while pos < len(data):
        code = data[pos]
        if code == 0:
            raise ValueError("zero inside COBS packet")
        pos += 1
        end = pos + code - 1
        if end > len(data):
            raise ValueError("truncated COBS block")
        out.extend(data[pos:end])
        pos = end
        if code != 0xFF and pos < len(data):
            out.append(0)
    return bytes(out)


def encode_packet(packet_type: PacketType, payload: bytes = b"", *, flags: int = 0, sequence: int = 0) -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    body = _HEADER.pack(VERSION, int(packet_type), flags, sequence & 0xFFFF, len(payload)) + payload
    body += _CRC.pack(crc32(body))
    return cobs_encode(body) + b"\x00"


def decode_packet(encoded: bytes) -> Packet:
    raw = cobs_decode(encoded)
    if len(raw) < _HEADER.size + _CRC.size:
        raise ValueError("packet too short")
    version, kind, flags, sequence, payload_len = _HEADER.unpack_from(raw)
    if version != VERSION:
        raise ValueError(f"unsupported version {version}")
    expected = _HEADER.size + payload_len + _CRC.size
    if payload_len > MAX_PAYLOAD or len(raw) != expected:
        raise ValueError("invalid payload length")
    body = raw[:-4]
    expected_crc = _CRC.unpack_from(raw, len(raw) - 4)[0]
    if crc32(body) != expected_crc:
        raise ValueError("CRC mismatch")
    return Packet(version, PacketType(kind), flags, sequence, raw[_HEADER.size:-4])


def encode_command(command: Command, *, sequence: int = 0) -> bytes:
    return encode_packet(PacketType.COMMAND, bytes([int(command)]), sequence=sequence)


def parse_can_frame(packet: Packet) -> CanFrame:
    if packet.packet_type != PacketType.CAN_FRAME or len(packet.payload) < 14:
        raise ValueError("not a CAN_FRAME")
    timestamp_us, identifier = struct.unpack_from("<QI", packet.payload)
    dlc = packet.payload[12]
    flags = packet.payload[13]
    rtr = bool(flags & 0x02)
    expected = 14 if rtr else 14 + dlc
    if dlc > 8 or len(packet.payload) != expected:
        raise ValueError("bad CAN frame length")
    return CanFrame(timestamp_us, identifier, dlc, bool(flags & 1), rtr,
                    bool(flags & 4), b"" if rtr else packet.payload[14:])


def parse_hello(packet: Packet) -> dict:
    if packet.packet_type != PacketType.HELLO or len(packet.payload) < 8:
        raise ValueError("not a HELLO")
    proto = packet.payload[0]
    bitrate = struct.unpack_from("<I", packet.payload, 1)[0]
    tx_enabled = bool(packet.payload[5])
    stream_enabled = bool(packet.payload[6])
    pos = 7
    name_len = packet.payload[pos]; pos += 1
    if pos + name_len + 1 > len(packet.payload):
        raise ValueError("truncated HELLO name")
    device = packet.payload[pos:pos + name_len].decode("utf-8", "replace")
    pos += name_len
    ver_len = packet.payload[pos]; pos += 1
    if pos + ver_len != len(packet.payload):
        raise ValueError("truncated HELLO version")
    firmware = packet.payload[pos:pos + ver_len].decode("utf-8", "replace")
    return dict(protocol=proto, bitrate=bitrate, tx_enabled=tx_enabled,
                stream_enabled=stream_enabled, device=device, firmware=firmware)


def parse_stats(packet: Packet) -> dict:
    if packet.packet_type != PacketType.STATS:
        raise ValueError("not STATS")
    fmt = struct.Struct("<QQQQQIIIIIBQ")
    if len(packet.payload) != fmt.size:
        raise ValueError("bad STATS size")
    keys = ("rx", "std", "ext", "rtr", "drop", "missed", "overrun",
            "buserr", "arb_lost", "driver_q", "state", "native_rx_errors")
    return dict(zip(keys, fmt.unpack(packet.payload), strict=True))
