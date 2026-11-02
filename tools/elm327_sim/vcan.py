"""Versioned Virtual CAN binary-over-USB-CDC wire codec (host only).

COBS + 0 delimiter, CRC32, little-endian, no host physical CAN API.
Wire v1 is an *implementation proposal* until firmware reviews it.
"""
from __future__ import annotations
from dataclasses import dataclass
import struct
import zlib
from .can import CANFrame, ECUSimulator

MAGIC = b"VCAN"
VERSION = (1, 0)
HELLO, CAN_TX, CAN_RX, STOP = 1, 2, 3, 4
HELLO_TOKEN = b"VIRTONLY"  # Not secret, proof of dedicated Virtual FW identity only
HEADER = struct.Struct("<4sBBBBIQIB")
CRC = struct.Struct("<I")
MAX_PACKET = 64


def cobs_encode(data: bytes) -> bytes:
    out = bytearray()
    idx = 0
    while idx < len(data):
        block = data[idx:idx + 254]
        zero = block.find(b"\x00")
        n = len(block) if zero == -1 else zero
        out.append(n + 1)
        out.extend(block[:n])
        idx += n + (1 if zero != -1 else 0)
    if not data or data.endswith(b"\x00"):
        out.append(1)
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    if not data:
        raise ValueError("empty COBS packet")
    out = bytearray()
    pos = 0
    while pos < len(data):
        code = data[pos]
        if code == 0:
            raise ValueError("embedded frame delimiter")
        pos += 1
        end = pos + code - 1
        if end > len(data):
            raise ValueError("COBS length overrun")
        out.extend(data[pos:end])
        pos = end
        if code < 255 and pos < len(data):
            out.append(0)
    return bytes(out)


@dataclass(frozen=True)
class VirtualFrame:
    kind: int
    seq: int
    timestamp_us: int
    can_id: int = 0
    data: bytes = b""
    extended: bool = False
    rtr: bool = False

    def __post_init__(self):
        if self.kind not in (HELLO, CAN_TX, CAN_RX, STOP):
            raise ValueError("invalid message type")
        if type(self.seq) is not int or not 0 <= self.seq <= 0xFFFFFFFF:
            raise ValueError("invalid sequence")
        if type(self.timestamp_us) is not int or not 0 <= self.timestamp_us <= 0xFFFFFFFFFFFFFFFF:
            raise ValueError("invalid monotonic timestamp")
        if not isinstance(self.data, bytes) or len(self.data) > 8:
            raise ValueError("invalid Classic CAN payload")
        if self.kind in (HELLO, STOP):
            if self.can_id != 0 or self.extended or self.rtr:
                raise ValueError("control frame cannot specify CAN ID")
            if self.kind == HELLO and self.data != HELLO_TOKEN:
                raise ValueError("unexpected Virtual-only HELLO token")
            if self.kind == STOP and self.data:
                raise ValueError("STOP must not carry data")
        else:
            CANFrame(self.can_id, self.data, self.extended)


def encode_packet(frame: VirtualFrame) -> bytes:
    flags = int(frame.extended) | (int(frame.rtr) << 1)
    body = HEADER.pack(MAGIC, *VERSION, frame.kind, flags, frame.seq,
                       frame.timestamp_us, frame.can_id, len(frame.data)) + frame.data
    raw = body + CRC.pack(zlib.crc32(body) & 0xFFFFFFFF)
    return cobs_encode(raw) + b"\x00"


def decode_packet(packet: bytes) -> VirtualFrame:
    if not packet.endswith(b"\x00") or packet.count(b"\x00") != 1:
        raise ValueError("missing/extra serial delimiter")
    if len(packet) > MAX_PACKET:
        raise ValueError("oversized wire packet")
    raw = cobs_decode(packet[:-1])
    if len(raw) < HEADER.size + CRC.size:
        raise ValueError("short wire packet")
    body, crc_bytes = raw[:-4], raw[-4:]
    if zlib.crc32(body) & 0xFFFFFFFF != CRC.unpack(crc_bytes)[0]:
        raise ValueError("CRC32 mismatch")
    magic, major, minor, kind, flags, seq, when, can_id, dlc = HEADER.unpack(body[:HEADER.size])
    if magic != MAGIC or (major, minor) != VERSION or flags & ~3:
        raise ValueError("unrecognized Virtual CAN frame version/flags")
    if len(body) != HEADER.size + dlc or dlc > 8:
        raise ValueError("DLC / body mismatch")
    return VirtualFrame(kind, seq, when, can_id, body[HEADER.size:],
                        bool(flags & 1), bool(flags & 2))


class VirtualCDCBridge:
    """Fails closed until an explicit Virtual-only HELLO; no real CAN driver."""
    def __init__(self, simulator: ECUSimulator):
        self.sim = simulator
        self.armed = False
        self.next_seq = 1
        self.peer_seq: int | None = None

    def _out(self, kind: int, timestamp_us: int, frame: CANFrame | None = None):
        result = VirtualFrame(kind, self.next_seq, timestamp_us,
                              frame.can_id if frame else 0,
                              frame.data if frame else (HELLO_TOKEN if kind == HELLO else b""),
                              frame.extended if frame else False)
        self.next_seq = (self.next_seq + 1) & 0xFFFFFFFF
        return encode_packet(result)

    def handle(self, packet: bytes) -> list[bytes]:
        try:
            item = decode_packet(packet)
        except ValueError:
            self.armed = False
            raise
        if item.kind == HELLO:
            self.armed = True
            self.peer_seq = item.seq
            return [self._out(HELLO, item.timestamp_us)]
        if not self.armed:
            raise ValueError("Virtual-only handshake required")
        if self.peer_seq is None or item.seq != ((self.peer_seq + 1) & 0xFFFFFFFF):
            self.armed = False
            raise ValueError("serial sequence mismatch; re-handshake required")
        self.peer_seq = item.seq
        if item.kind == STOP:
            self.armed = False
            return []
        if item.kind != CAN_TX or item.rtr:
            self.armed = False
            raise ValueError("expected virtual CAN_TX data frame only")
        return [self._out(CAN_RX, item.timestamp_us, frame)
                for frame in self.sim.on_frame(CANFrame(item.can_id, item.data, item.extended),
                                              item.timestamp_us // 1000)]

    def tick(self, now_us: int) -> list[bytes]:
        if not self.armed:
            return []
        return [self._out(CAN_RX, now_us, frame)
                for frame in self.sim.tick(now_us // 1000)]
