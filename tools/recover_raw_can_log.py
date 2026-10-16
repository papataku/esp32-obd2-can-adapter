#!/usr/bin/env python3
"""Decode or recover lossless M5CAN RAW v1 blocks; no device flash writes.

Input is either a single MCAN binary block or a captured USB console text
from CANLOG READ. Output is sorted CAN event observations, not ECU timestamps.
"""
import argparse
import csv
import json
import re
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path

HEADER = 32
MAX_PAYLOAD = 3584
BLOCK = re.compile(r'^#CANLOG,BLOCK,index=(\d+),type=(\d+),length=(\d+)$')
DATA = re.compile(r'^#CANLOG,DATA,index=(\d+),offset=(\d+),hex=([0-9a-fA-F]*)$')
END = re.compile(r'^#CANLOG,BLOCKEND,index=(\d+)$')


class RawCanError(ValueError):
    pass


def varuint(buf: bytes, cursor: int):
    value = 0
    for i in range(10):
        if cursor >= len(buf):
            raise RawCanError('truncated varint')
        b = buf[cursor]
        cursor += 1
        if i == 9 and b & 0xFE:
            raise RawCanError('varint overflow')
        value |= (b & 0x7F) << (7 * i)
        if not b & 0x80:
            return value, cursor
    raise RawCanError('overlong varint')


@dataclass(frozen=True)
class RawCanEvent:
    sequence: int
    timestamp_us: int
    can_id: int
    extended: bool
    remote: bool
    self_reception: bool
    direction: str
    data: bytes

    def as_csv(self):
        return {
            'sequence': self.sequence,
            'adapter_observed_us': self.timestamp_us,
            'can_id': f'{self.can_id:08X}' if self.extended else f'{self.can_id:03X}',
            'extended': int(self.extended),
            'rtr': int(self.remote),
            'self': int(self.self_reception),
            'direction': self.direction,
            'dlc': len(self.data),
            'data_hex': self.data.hex().upper(),
        }


def decode(raw: bytes):
    if len(raw) < HEADER or raw[:4] != b'MCAN' or raw[4:6] != b'\x01\x00':
        raise RawCanError('unknown block format/version')
    count, first_seq = struct.unpack_from('<HI', raw, 6)
    first_time, encoded, crc, reserved = struct.unpack_from('<QIII', raw, 12)
    if not count or not first_seq or encoded > MAX_PAYLOAD or reserved:
        raise RawCanError('invalid header')
    if len(raw) != HEADER + encoded:
        raise RawCanError('truncated/extended encoded block')
    observed = zlib.crc32(raw[:24] + raw[32:]) & 0xFFFFFFFF
    if observed != crc:
        raise RawCanError('CRC mismatch')
    cursor = HEADER
    events = []
    seq, timestamp, prior_id, prior_payload = first_seq, first_time, 0, b''
    for n in range(count):
        seq_delta, cursor = varuint(raw, cursor)
        delta_us, cursor = varuint(raw, cursor)
        if cursor + 2 > len(raw):
            raise RawCanError('truncated frame metadata')
        meta, dlc = raw[cursor:cursor + 2]
        cursor += 2
        if meta & 0x80 or (meta & 3) > 2 or dlc > 8:
            raise RawCanError('invalid direction/flags/DLC')
        if n == 0 and (seq_delta or delta_us):
            raise RawCanError('bad block base')
        if n > 0 and (not seq_delta or seq + seq_delta > 0xFFFFFFFF):
            raise RawCanError('non-monotonic sequence')
        if n:
            seq += seq_delta
            timestamp += delta_us
        if timestamp > 0xFFFFFFFFFFFFFFFF:
            raise RawCanError('timestamp overflow')
        if meta & 0x20:
            if n == 0:
                raise RawCanError('first frame reuses ID')
            identifier = prior_id
        else:
            identifier, cursor = varuint(raw, cursor)
        extended, remote, self_reception = bool(meta & 4), bool(meta & 8), bool(meta & 16)
        if identifier > (0x1FFFFFFF if extended else 0x7FF):
            raise RawCanError('invalid CAN identifier')
        if meta & 0x40:
            if n == 0 or len(prior_payload) != dlc:
                raise RawCanError('invalid previous-data reference')
            payload = prior_payload
        else:
            if cursor + dlc > len(raw):
                raise RawCanError('truncated CAN frame payload')
            payload = raw[cursor:cursor + dlc]
            cursor += dlc
        event = RawCanEvent(seq, timestamp, identifier, extended, remote,
                            self_reception, ('RX', 'TX_SUBMITTED', 'TX_SUBMIT_FAILED')[meta & 3], payload)
        events.append(event)
        prior_id, prior_payload = identifier, payload
    if cursor != len(raw):
        raise RawCanError('unexpected bytes at end of block')
    return events


def recover_usb(text: str):
    records = []
    active = None
    content = bytearray()
    for line in text.splitlines():
        line = line.strip()
        m = BLOCK.fullmatch(line)
        if m:
            if active is not None:
                raise RawCanError('new BLOCK before previous BLOCKEND')
            index, kind, expected = map(int, m.groups())
            if index != len(records) or kind not in (1, 2):
                raise RawCanError('block index gap/type')
            active = (index, kind, expected)
            content = bytearray()
            continue
        m = DATA.fullmatch(line)
        if m:
            if active is None or int(m[1]) != active[0] or int(m[2]) != len(content):
                raise RawCanError('missing/duplicate/out-of-order block chunk')
            content.extend(bytes.fromhex(m[3]))
            if len(content) > active[2]:
                raise RawCanError('block length overflow')
            continue
        m = END.fullmatch(line)
        if m:
            if active is None or int(m[1]) != active[0] or len(content) != active[2]:
                raise RawCanError('truncated/incorrect block completion')
            if active[1] == 1:
                decode(bytes(content))  # validate block before accepting
            elif content:
                raise RawCanError('nonempty END marker')
            records.append((active[1], bytes(content)))
            active = None
    if active is not None or not records:
        raise RawCanError('no complete block set')
    if not any(kind == 2 for kind, _ in records):
        # We still expose salvaged complete blocks but never claim a clean STOP.
        return records, False
    if records[-1][0] != 2 or any(k == 2 for k, _ in records[:-1]):
        raise RawCanError('END marker in middle of session')
    return records, True


def export(records, out: Path, complete: bool):
    out.mkdir(parents=True, exist_ok=True)
    blockdir = out / 'blocks'
    blockdir.mkdir(exist_ok=True)
    entries = []
    for index, (kind, raw) in enumerate(records):
        if kind == 1:
            (blockdir / f'{index:06d}.mcan').write_bytes(raw)
            entries.extend(decode(raw))
    if any(b.sequence <= a.sequence for a, b in zip(entries, entries[1:])):
        raise RawCanError('nonmonotonic sequence across blocks')
    csv_path = out / 'can_frames.csv'
    with csv_path.open('w', newline='', encoding='utf-8') as fp:
        writer = csv.DictWriter(fp, fieldnames=[
            'sequence', 'adapter_observed_us', 'can_id', 'extended', 'rtr',
            'self', 'direction', 'dlc', 'data_hex'])
        writer.writeheader()
        writer.writerows(e.as_csv() for e in entries)
    gaps = sum(max(0, b.sequence - a.sequence - 1) for a, b in zip(entries, entries[1:]))
    summary = {
        'format': 'M5CAN RAW v1',
        'blocks': len(records), 'events': len(entries),
        'observed_sequence_gaps': gaps,
        'stop_marker_seen': complete,
        'integrity': 'verified_blocks_only',
        'note': 'Missing TWAI driver frames require separate hardware counter evidence',
    }
    (out / 'manifest.json').write_text(
        json.dumps(summary, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    return summary


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('input', type=Path, help='CANLOG USB capture or one .mcan block')
    p.add_argument('--output', required=True, type=Path)
    args = p.parse_args()
    if args.input.suffix.lower() in ('.mcan', '.bin'):
        records, complete = [(1, args.input.read_bytes())], False
    else:
        records, complete = recover_usb(args.input.read_text(encoding='utf-8'))
    print(json.dumps(export(records, args.output, complete), indent=2))


if __name__ == '__main__':
    main()
