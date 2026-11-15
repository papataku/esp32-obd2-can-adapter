#!/usr/bin/env python3
"""Offline, read-only decoder of the Signal V2 flight recorder (PM Claude 2026-11-15).

Inputs (never writes to any device; never opens a serial port):
  * a raw dump of the 'spiffs' EvidenceStore partition (0x180000 bytes), or
  * a full 8 MiB flash backup (esptool read_flash 0 0x800000); the partition is
    taken from --offset (default 0x670000 when the file is exactly 8 MiB), or
  * --text: a USB log containing `V2REC DUMP HEX` output (#V2REC_HEX lines).
Output: JSON (default) or --csv <file> with one row per SUMMARY/SUMMARY2 pair.
Layout mirrors include/signal_v2_flight_recorder.h (format version 1).
"""
import argparse, json, re, struct, sys, zlib

OWNER, RECORD, COMMIT = 0x4D354F57, 0x4D354552, 0x434F4D54
STORE_HEADER = struct.Struct('<IHHQIIII')
V2REC_TYPE, SLOTS = 4, 48
PAGE_MAGIC, PAGE_HEADER = 0x50523256, struct.Struct('<IHHIIHHHHI')
PIDS = ['0D', '0C', '05', '5B', '9A']
PID_FIELDS = ['demand', 'admitted', 'completed', 'failed', 'shed', 'denied', 'cancelled']
EVENTS = {1: 'ARM', 2: 'DISARM', 3: 'MANUAL_STOP', 4: 'ECU_PIN', 5: 'ECU_UNPIN', 6: 'LINK_UP',
          7: 'LINK_DOWN', 8: 'LINK_SUB', 9: 'LINK_UNSUB', 10: 'STOP_IDLE', 12: 'RING_DROP',
          13: 'FLUSH', 14: 'STORE_FAIL', 15: 'BOOT_SUPPRESSED', 16: 'AUTH_END', 17: 'ECU_STATE'}
AUTH = {1: 'SESSION_ENDED', 2: 'USB_ARM_EXPIRED', 3: 'OPERATOR_STOP', 4: 'CONTROL_FIFO_OVERFLOW',
        5: 'CENTRAL_UNSUBSCRIBE', 6: 'AUTO_ARM_GRANTED'}
STATUS = ['ok', 'nodata', 'busy', 'lease', 'invalid', 'rate', 'driver', 'busoff', 'mode', 'isotp']
LAT = ['lt10', 'lt20', 'lt30', 'lt50', 'lt80', 'lt120', 'ge120', 'none']
BLE = ['CONNECT', 'DISCONNECT', 'REJECT', 'ERROR']
SITES = ['transition', 'stale', 'control', 'expired']


class R:
    def __init__(self, b): self.b, self.n = b, 0
    def take(self, fmt):
        v = struct.unpack_from('<' + fmt, self.b, self.n); self.n += struct.calcsize('<' + fmt)
        return v if len(v) > 1 else v[0]
    def text(self, n):
        s = self.b[self.n:self.n + n]; self.n += n
        return s.split(b'\0')[0].decode('ascii', 'replace')


def flags(v):
    return {k: bool(v & (1 << i)) for i, k in enumerate(
        ['armed', 'stopped', 'busy', 'manual', 'pinned', 'link0', 'link1', 'subscribed'])}


def record(t, p):
    r = R(p); out = {}
    if t == 1:
        out = dict(type='BOOT', boot=r.take('I'), first_page_seq=r.take('I'), reset=r.take('B'),
                   rom_reset=r.take('B'), abnormal_streak=r.take('B'))
        f = r.take('B'); out.update(store_ready=bool(f & 1), boot_page_suppressed=bool(f & 2),
                                    retained_pages=r.take('H'), git=r.text(40))
    elif t == 2:
        ms, code, a8, a16, a32 = r.take('IBBHI')
        out = dict(type='EVENT', ms=ms, event=EVENTS.get(code, code), a8=a8, a16=a16, a32=a32)
        if code == 16: out['auth'] = AUTH.get(a8, a8)
        if code in (4, 5): out['ecu_response_id'] = '%08X' % a32
    elif t == 3:
        out = dict(type='STOP', ms=r.take('I'), epoch=r.take('I'))
        out['flags'] = flags(r.take('B')); out['last_quality'] = r.take('B')
        out['source'] = '%08X' % r.take('I'); out['acquired'] = r.take('I'); out['invalid'] = r.take('I')
        out['age_ms'] = dict(zip(PIDS, r.take('5H')))
        for k in ['last_status', 'last_responses', 'last_raw', 'ecu_state', 'ecu_bad']: out[k] = r.take('B')
        out['nodata_run'] = r.take('H'); out['nodata_run_max'] = r.take('H')
        out['reason'] = r.text(r.take('B')); out['can_last_error'] = r.text(r.take('B'))
    elif t == 4:
        out = dict(type='SUMMARY', ms=r.take('I'), period_ms=r.take('I'))
        out['flags'] = flags(r.take('B')); out['last_quality'] = r.take('B'); out['epoch'] = r.take('I')
        out['pid'] = {}
        for pid in PIDS:
            v = r.take('9H'); d = dict(zip(PID_FIELDS, v[:7])); d['samples'] = v[7]; d['max_gap_ms'] = v[8]
            out['pid'][pid] = d
        for k in ['acquired', 'invalid', 'inv_nodata', 'inv_verify', 'inv_stop', 'inv_other']: out[k] = r.take('H')
        out['ecu_state'] = r.take('B'); out['ecu_bad'] = r.take('B')
        out['nodata_run_win_max'] = r.take('H'); out['nodata_run_boot_max'] = r.take('H')
    elif t == 6:
        out = dict(type='SUMMARY2', ms=r.take('I'))
        for k in ['tx_attempts', 'tx_success', 'query', 'query_ok', 'query_timeout', 'query_no_data',
                  'query_failed', 'v2_rx', 'v2_fc', 'app_queue_drops', 'nrc']: out[k] = r.take('H')
        out['status'] = dict(zip(STATUS, r.take('10H')))
        for k in ['rx_missed', 'rx_overrun', 'bus_error']: out[k] = r.take('H')
        out['twai_state'] = r.take('B'); out['rx_queue'] = r.take('B'); out['fault_locked'] = bool(r.take('B'))
        for k in ['data_pkts', 'snapshot_pkts', 'other_pkts', 'snapshot_empty', 'snapshot_records',
                  'op4_ok', 'op4_rej', 'op7_ok', 'op7_rej', 'other_ok', 'other_rej']: out[k] = r.take('H')
        out['last_rej_op'] = r.take('B'); out['last_rej_code'] = r.take('B')
        out['discard'] = {s: dict(zip(['data', 'snapshot', 'other'], r.take('3H'))) for s in SITES}
        out['results'] = r.take('H'); out['latency'] = dict(zip(LAT, r.take('8H'))); out['multi_frame'] = r.take('H')
        for k in ['last_status', 'last_responses', 'last_raw', 'last_sf', 'last_ff', 'last_cf']: out[k] = r.take('B')
        for k in ['last_first_us', 'last_ff_us', 'last_cf_us']: out[k] = r.take('I')
    elif t == 5:
        out = dict(type='LIVE', ms=r.take('I'), epoch=r.take('I'))
        out['flags'] = flags(r.take('B')); out['last_quality'] = r.take('B')
        out['acquired'] = r.take('I'); out['invalid'] = r.take('I'); out['source'] = '%08X' % r.take('I')
        for k in ['inv_nodata', 'inv_verify', 'inv_stop', 'inv_other']: out[k] = r.take('I')
        out['pid'] = {}
        for pid in PIDS:
            v = r.take('8I'); d = dict(zip(PID_FIELDS, v[:7])); d['samples'] = v[7]; d['max_gap_ms'] = r.take('H')
            out['pid'][pid] = d
        out['data_pkts'] = r.take('I'); out['snapshot_pkts'] = r.take('I')
        for k in ['op4_ok', 'op4_rej', 'op7_ok', 'op7_rej']: out[k] = r.take('H')
        out['ring_drops'] = r.take('I'); out['nodata_results'] = r.take('H'); out['nodata_run_max'] = r.take('H')
    elif t == 7:
        ms, kind, known, conn, gen, reason, mtu, itv, lat, sup, code = r.take('IBBHIiHHHHH')
        out = dict(type='BLE', ms=ms, kind=BLE[kind] if kind < 4 else kind, reason_known=bool(known),
                   conn=conn, gen=gen, reason=reason, mtu=mtu, interval=itv, latency=lat,
                   supervision=sup, code=code)
    elif t == 8:
        out = dict(type='TEXT', ms=r.take('I')); k = r.take('B')
        out['kind'] = 'CAN_LAST_ERROR' if k == 1 else k; out['text'] = r.text(r.take('B'))
    else:
        out = dict(type='unknown_%d' % t)
    if r.n != len(p): out['length_mismatch'] = [r.n, len(p)]
    return out


def page(blob):
    if len(blob) < PAGE_HEADER.size: return None
    magic, ver, kind, seq, boot, pib, records, used, fl, dropped = PAGE_HEADER.unpack_from(blob)
    if magic != PAGE_MAGIC or ver != 1 or kind not in (1, 2) or PAGE_HEADER.size + used > len(blob):
        return None
    out = dict(page_seq=seq, boot=boot, kind='BOOT' if kind == 1 else 'FLUSH', page_in_boot=pib,
               flags=dict(overflow=bool(fl & 1), head=bool(fl & 2), rewrite=bool(fl & 4)),
               dropped_before=dropped, records=[])
    at = PAGE_HEADER.size; end = at + used
    while at + 2 <= end:
        t, n = blob[at], blob[at + 1]
        if at + 2 + n > end: out['truncated'] = True; break
        out['records'].append(record(t, blob[at + 2:at + 2 + n])); at += 2 + n
    if len(out['records']) != records: out['record_count_mismatch'] = [len(out['records']), records]
    return out


def scan_partition(data):
    if len(data) % 4096 or len(data) < 3 * 4096: raise ValueError('sector-aligned partition required')
    owners = 0
    for off in (0, 4096):
        h = STORE_HEADER.unpack_from(data, off)
        if h[:5] == (OWNER, 1, 0, 0, 0) and h[5] == len(data) and h[7] == COMMIT and \
                h[6] == zlib.crc32(data[off:off + 24]):
            owners += 1
    if not owners: raise ValueError('no CRC-valid EvidenceStore owner header; refusing foreign data')
    pages, other = [], {}
    for off in range(8192, len(data), 4096):
        magic, ver, kind, seq, slot, size, crc, commit = STORE_HEADER.unpack_from(data, off)
        if magic != RECORD or commit != COMMIT or ver != 1 or not 0 < size <= 2304: continue
        payload = data[off + 32:off + 32 + size]
        if zlib.crc32(data[off:off + 24] + payload) != crc: continue
        if kind != V2REC_TYPE:
            other[kind] = other.get(kind, 0) + 1; continue
        if slot >= SLOTS: continue
        p = page(payload)
        if p: p.update(store_sequence=seq, slot=slot, offset=off); pages.append(p)
    return pages, {'owner_headers': owners, 'other_record_types': other}


def scan_text(text):
    chunks = {}
    for m in re.finditer(r'#V2REC_HEX,src=(\w+),page_seq=(\d+),off=(\d+),data=([0-9A-F]+)', text):
        key = (m.group(1), int(m.group(2)))
        chunks.setdefault(key, {})[int(m.group(3))] = bytes.fromhex(m.group(4))
    pages = []
    for (src, _), parts in sorted(chunks.items()):
        p = page(b''.join(parts[k] for k in sorted(parts)))
        if p: p['source'] = src; pages.append(p)
    return pages, {}


def build(pages, meta):
    latest = {}
    for p in pages:  # a later rewrite of the same RAM page supersedes an earlier copy
        k = (p['boot'], p['page_in_boot'], p.get('source', 'flash'))
        if k not in latest or p['page_seq'] > latest[k]['page_seq']: latest[k] = p
    unique = sorted(latest.values(), key=lambda p: (p['boot'], p['page_in_boot'], p['page_seq']))
    boots = {}
    for p in unique:
        b = boots.setdefault(p['boot'], {'boot': p['boot'], 'pages': 0, 'records': [], 'stopped': None})
        b['pages'] += 1
        for r in p['records']:
            if p['page_in_boot'] and r['type'] == 'BOOT': continue  # head copy of the boot record
            b['records'].append(r)
            if r['type'] == 'STOP': b['stopped'] = r['reason']
    for b in boots.values():
        hist = {}
        for r in b['records']:
            if r['type'] == 'SUMMARY': hist[r['ms'] // 60000] = hist.get(r['ms'] // 60000, 0) + r['inv_nodata']
        b['nodata_per_minute'] = {str(k): v for k, v in sorted(hist.items())}
    return {'format': 'v2rec-1', **meta, 'pages_total': len(pages), 'pages_unique': len(unique),
            'boots': [boots[k] for k in sorted(boots)]}


def csv_rows(result):
    rows = []
    for b in result['boots']:
        pending = None
        for r in b['records']:
            if r['type'] == 'SUMMARY': pending = r
            elif r['type'] == 'SUMMARY2' and pending:
                row = {'boot': b['boot'], 'ms': pending['ms'], 'period_ms': pending['period_ms'],
                       'armed': int(pending['flags']['armed']), 'stopped': int(pending['flags']['stopped'])}
                for pid, d in pending['pid'].items():
                    for k, v in d.items(): row['%s_%s' % (pid, k)] = v
                for k in ['acquired', 'invalid', 'inv_nodata', 'inv_verify', 'inv_stop', 'inv_other',
                          'ecu_state', 'ecu_bad', 'nodata_run_win_max']: row[k] = pending[k]
                def flat(prefix, v):
                    if isinstance(v, dict):
                        for kk, vv in v.items(): flat(prefix + '_' + kk if prefix else kk, vv)
                    else: row[prefix] = v
                for k, v in r.items():
                    if k not in ('type', 'ms'): flat(k, v)
                rows.append(row); pending = None
    return rows


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('input'); ap.add_argument('--text', action='store_true')
    ap.add_argument('--offset', type=lambda s: int(s, 0)); ap.add_argument('--size', type=lambda s: int(s, 0), default=0x180000)
    ap.add_argument('--csv'); ap.add_argument('--output')
    a = ap.parse_args(argv)
    if a.text:
        pages, meta = scan_text(open(a.input, encoding='utf-8', errors='replace').read())
    else:
        data = open(a.input, 'rb').read()
        off = a.offset if a.offset is not None else (0x670000 if len(data) == 0x800000 else 0)
        pages, meta = scan_partition(data[off:off + a.size] if off or len(data) > a.size else data)
    result = build(pages, meta)
    if a.csv:
        import csv
        rows = csv_rows(result)
        keys = []
        for row in rows:
            keys += [k for k in row if k not in keys]
        with open(a.csv, 'w', newline='') as f:
            w = csv.DictWriter(f, fieldnames=keys); w.writeheader(); w.writerows(rows)
    text = json.dumps(result, indent=1, ensure_ascii=False)
    if a.output: open(a.output, 'w').write(text)
    else: print(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
