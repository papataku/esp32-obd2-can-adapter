#!/usr/bin/env python3
"""Offline, read-only forensic scan of the owned evidence partition.

Old committed revisions may remain until sector rotation; they are historical
evidence, not current key values. Never use this tool to format or repair flash.
"""
import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path

OWNER, RECORD, COMMIT = 0x4D354F57, 0x4D354552, 0x434F4D54
HEADER = struct.Struct('<IHHQIIII')
ENTRY = struct.Struct('<IIIHHHHHHiBBHIIHHIQQ')


def fnv(data):
    result = 2166136261
    for byte in data: result = ((result ^ byte)*16777619) & 0xffffffff
    return result


def decode_ble(data):
    if len(data) != 2184 or struct.unpack_from('<IHH', data) != (0x4D35424C, 3, 2184):
        return {'unsupported_schema': True}
    raw = bytearray(data); stored = struct.unpack_from('<I', raw, 8)[0]
    raw[8:12] = b'\0'*4
    if fnv(raw) != stored: return {'inner_checksum_invalid': True}
    count = data[96]
    if count > 12: return {'invalid_event_count': count}
    values = struct.unpack_from('<11I', data, 12)
    result = dict(zip(['boot', 'saves', 'total', 'evicted', 'queue_drops', 'first_seq',
                       'last_seq', 'seq_gaps', 'unknown_faults', 'storage_errors', 'peer_overflow'], values))
    result['build'] = data[100:148].split(b'\0')[0].decode('ascii', errors='replace')
    keys = ['seq', 'ms', 'gen', 'conn', 'peer_suffix', 'mtu', 'interval', 'latency',
            'supervision', 'reason_raw', 'kind', 'reason_known', 'reserved', 'retries',
            'failed', 'code', 'reserved2', 'detail', 'rx_bytes', 'tx_bytes']
    result['events'] = [dict(zip(keys, ENTRY.unpack_from(data, 152+64*i))) for i in range(count)]
    result['faults'] = []
    for i in range(48):
        code, _, occurrences, first, last, a, b = struct.unpack_from('<HHIIIII', data, 920+24*i)
        if code: result['faults'].append({'code': code, 'count': occurrences, 'first_ms': first,
                                        'last_ms': last, 'first_detail': a, 'last_detail': b})
    result['disconnect_peers'] = []
    for i in range(4):
        peer, conn, gen, occurrences, first, last, a, b = struct.unpack_from('<HHIIIIii', data, 2072+28*i)
        if occurrences: result['disconnect_peers'].append({'peer_suffix': peer, 'conn': conn,
            'gen': gen, 'count': occurrences, 'first_ms': first, 'last_ms': last, 'first_reason': a, 'last_reason': b})
    return result


def inspect(data):
    if len(data) < 3*4096 or len(data) % 4096: raise ValueError('whole sector-aligned partition required')
    owners = 0
    for offset in [0, 4096]:
        h = HEADER.unpack_from(data, offset)
        if h[:5] == (OWNER, 1, 0, 0, 0) and h[5] == len(data) and h[7] == COMMIT and h[6] == zlib.crc32(data[offset:offset+24]): owners += 1
    if not owners: raise ValueError('no CRC-valid ownership header; refuse foreign data')
    records, invalid, uncommitted = [], 0, 0
    for offset in range(8192, len(data), 4096):
        magic, version, kind, sequence, slot, size, crc, commit = HEADER.unpack_from(data, offset)
        if magic == 0xffffffff: continue
        if commit != COMMIT: uncommitted += 1; continue
        if magic != RECORD or version != 1 or not sequence or kind not in [1, 2, 3] or slot >= (4 if kind == 3 else 8) or not 0 < size <= 2304:
            invalid += 1; continue
        payload = data[offset+32:offset+32+size]
        if zlib.crc32(data[offset:offset+24]+payload) != crc:
            invalid += 1; continue
        record = {'offset': offset, 'sequence': sequence, 'type': kind, 'slot': slot, 'bytes': size}
        if kind == 3: record['ble'] = decode_ble(payload)
        records.append(record)
    records.sort(key=lambda r: r['sequence'])
    latest = {}
    for r in records: latest[(r['type'], r['slot'])] = r['sequence']
    for r in records: r['current_key_revision'] = r['sequence'] == latest[(r['type'], r['slot'])]
    return {'sha256': hashlib.sha256(data).hexdigest(), 'owner_headers': owners,
            'invalid_sectors': invalid, 'uncommitted_sectors': uncommitted, 'records': records,
            'ble_inner_invalid': sum(bool(r.get('ble', {}).get('inner_checksum_invalid')) for r in records),
            'ble_unsupported': sum(bool(r.get('ble', {}).get('unsupported_schema')) for r in records),
            'limits': 'Historical committed sectors are not guaranteed retention. BLE v3 only; outer CRC and inner FNV checked independently.'}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('partition', type=Path); p.add_argument('--output', required=True, type=Path)
    args = p.parse_args(); result = inspect(args.partition.read_bytes())
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'records'}))
    print('CRC-valid committed revisions:', len(result['records']))
