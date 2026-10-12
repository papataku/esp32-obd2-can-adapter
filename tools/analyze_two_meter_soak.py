#!/usr/bin/env python3
"""Offline two-meter acceptance; measured UTC/monotonic window excludes cleanup."""
import argparse
import json
import re
from pathlib import Path


def fields(line):
    return dict(re.findall(r'([A-Za-z0-9_]+)=([^,\s]+)', line))


def summarize(values):
    values = sorted(values)
    return {'samples': len(values), 'min': values[0], 'p50': values[len(values)//2],
            'max': values[-1]} if values else None


def analyze(root):
    manifest = json.loads((root/'manifest.json').read_text())
    lo = manifest['measurement_start_monotonic']
    hi = manifest.get('measurement_end_monotonic')
    rows = []
    partial = 0
    with (root/'mac_events.jsonl').open() as f:
        for line in f:
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                partial += 1
                continue
            if row['monotonic'] >= lo and (hi is None or row['monotonic'] <= hi):
                rows.append(row)
    signals, peers, series, counters, policies = {}, {}, {}, {}, {}
    reboots, malformed = [], {}
    for row in rows:
        device, line = row['device'], row['line']
        d = fields(line)
        def numeric(required):
            return all(k in d and d[k].isdigit() for k in required)
        def invalid(kind):
            key = device+':'+kind
            malformed[key] = malformed.get(key, 0)+1
        if 'ESP-ROM:' in line or 'Guru Meditation' in line or 'panic' in line.lower():
            reboots.append({'device': device, 'utc': row['utc'], 'line': line})
        if 'M5CAN SIGNAL ' in line:
            if re.fullmatch(r'M5CAN SIGNAL id=\d+ value_x100=-?\d+ quality=\d+ updates=\d+ age_ms=\d+', line):
                signals.setdefault(device+':'+d['id'], []).append((row['monotonic'], d))
            else: invalid('signal')
        if line.startswith('M5CAN DISPLAY_POLL '):
            if re.fullmatch(r'M5CAN DISPLAY_POLL mask=\d+ auto=[01] background=\d+ foreground=\d+/\d+/\d+/\d+/\d+',line):
                policies.setdefault(device,[]).append((row['monotonic'],d))
            else: invalid('policy')
        if '#BLE_PEER,' in line:
            if re.fullmatch(r'#BLE_PEER,slot=\d+,up=[01],id=\d+,gen=\d+,addr=[0-9A-Fa-f]+,mtu=\d+,rx_bytes=\d+,tx_bytes=\d+,rx_writes=\d+,notify=\d+,retries=\d+,failed=\d+', line):
                peers.setdefault(d['id']+':'+d['gen'], []).append((row['monotonic'], d))
            else: invalid('peer')
        if '#BLE_DUAL,' in line:
            keys = ['clients', 'connects', 'disconnects', 'rx_overflow', 'notify', 'retries', 'failed']
            if re.fullmatch(r'#BLE_DUAL,clients=\d+,connects=\d+,disconnects=\d+,rx_overflow=\d+,notify=\d+,retries=\d+,failed=\d+', line):
                for k in keys: counters.setdefault('adapter:'+k, []).append((row['monotonic'], int(d[k])))
            else: invalid('dual')
        for tag, pattern, counter_keys, range_keys in [
            ('#BLE_MEMORY,', r'#BLE_MEMORY,alloc_failed=\d+,api_failed=\d+,min_free=\d+,last_ms=\d+,last_conn=\d+,last_rc=-?\d+',
             ['alloc_failed', 'api_failed'], ['min_free']),
            ('#BLE_FLOW,', r'#BLE_FLOW,waits=\d+,queue_peak=\d+,host_peak=\d+(?:,host_queue_peak=\d+)?,overflow=\d+,stalls=\d+',
             ['waits', 'overflow', 'stalls'], ['queue_peak', 'host_peak', 'host_queue_peak'])]:
            if line.startswith(tag):
                if re.fullmatch(pattern, line):
                    for k in counter_keys:
                        counters.setdefault('adapter:'+k, []).append((row['monotonic'], int(d[k])))
                    for k in range_keys:
                        if k not in d: continue
                        series.setdefault('adapter:'+k, []).append(int(d[k]))
                else: invalid(tag[1:-1])
        if line.startswith('POLL '):
            match = re.match(r'POLL (\d+) ', line)
            if match and re.fullmatch(r'POLL \d+ enabled=[01] period=\d+ ok=\d+ timeout=\d+ error=\d+ latency=\d+', line):
                for k in ['ok', 'timeout', 'error']:
                    counters.setdefault(device+':poll'+match[1]+':'+k, []).append((row['monotonic'], int(d[k])))
                series.setdefault(device+':poll'+match[1]+':latency_ms', []).append(int(d['latency']))
            else: invalid('poll')
        if line.startswith('M5CAN PID '):
            match = re.fullmatch(r'M5CAN PID (\d+) target=(\d+)Hz actual=([0-9.]+)Hz missed=(\d+)', line)
            if match:
                series.setdefault(device+':poll'+match[1]+':reported_hz', []).append(float(match[3]))
                counters.setdefault(device+':poll'+match[1]+':missed', []).append((row['monotonic'], int(match[4])))
            else: invalid('pid_rate')
        if line.startswith(('BLE_HEALTH ', '#MEMORY,', '#BLE_TASK,')):
            for k in ['internal_free', 'internal_min', 'largest', 'stack_min_free', 'stack_free_bytes',
                      'can_stack_free_bytes', 'rx_dropped', 'connected', 'poll_peak_us', 'ui_loop_peak_us']:
                if k in d and d[k].isdigit():
                    series.setdefault(device+':'+k, []).append(int(d[k]))
        if line.startswith('FPS '):
            for k in ['dma', 'ui']:
                if k in d:
                    try: series.setdefault(device+':fps_'+k, []).append(float(d[k]))
                    except ValueError: invalid('fps')
        if line.startswith('TACOLOG {'):
            try:
                record, _ = json.JSONDecoder().raw_decode(line[len('TACOLOG '):])
                if record.get('type') == 'saved':
                    series.setdefault(device+':save_us', []).append(record['write_us'])
                    series.setdefault(device+':save_failed', []).append(int(not record['ok']))
            except (ValueError, KeyError): invalid('tacolog')
    rates = {}
    for key, samples in signals.items():
        start, a = samples[0]; end, b = samples[-1]
        delta = int(b['updates'])-int(a['updates'])
        rates[key] = {'samples': len(samples), 'sample_s': round(end-start, 3),
                      'updates_delta': delta, 'hz': round(delta/(end-start), 3) if end > start and delta >= 0 else None,
                      'max_age_ms': max(int(d['age_ms']) for _, d in samples),
                      'qualities': sorted(set(int(d['quality']) for _, d in samples))}
        windows = []
        for offset in range(0, int(end-lo), 300):
            subset = [(t, d) for t, d in samples if lo+offset <= t < lo+offset+300]
            if len(subset) < 2: continue
            first, a = subset[0]; last, b = subset[-1]
            if last > first:
                windows.append({'offset_s': offset, 'sample_s': round(last-first, 3),
                                'hz': round((int(b['updates'])-int(a['updates']))/(last-first), 3)})
        rates[key]['five_minute_windows'] = windows
    policy_report={}
    for device,samples in policies.items():
        changes=[];previous=None;last_high=lo;initial_high_end=hi or rows[-1]['monotonic'];ended=False
        for timestamp,d in samples:
            state=(d['auto'],d['foreground'],d['mask'],d['background'])
            if state!=previous:
                changes.append({'offset_s':round(timestamp-lo,3),'auto':int(d['auto']),'foreground':d['foreground'],'mask':int(d['mask']),'background':int(d['background'])})
                previous=state
            matched=d['auto']=='0' and d['foreground']=='20/20/20/1/1'
            if manifest.get('meter_bench_preset')=='high' and not ended:
                if matched:last_high=timestamp
                else:initial_high_end=last_high;ended=True
        fixed_rates={}
        if manifest.get('meter_bench_preset')=='high':
            for signal in ['0','1','5']:
                subset=[(t,d) for t,d in signals.get(device+':'+signal,[]) if t<=initial_high_end]
                if len(subset)>1:
                    t0,a=subset[0];t1,b=subset[-1]
                    fixed_rates[signal]={'sample_s':round(t1-t0,3),'hz':round((int(b['updates'])-int(a['updates']))/(t1-t0),3)}
        policy_report[device]={'changes':changes,'initial_matched_high_s':round(initial_high_end-lo,3) if manifest.get('meter_bench_preset')=='high' else None,'initial_matched_high_rates':fixed_rates,'comparison_note':'A changed automatic/foreground policy changes the intended sensor rates; overall rates must not be labeled a transport regression. Transition timing is sampled; initial matched interval ends at the last confirmed high snapshot.'}
    traffic = {}
    for key, samples in peers.items():
        start, a = samples[0]; end, b = samples[-1]
        traffic[key] = {'sample_s': round(end-start, 3), 'samples': len(samples),
                        **{k+'_per_s': round((int(b[k])-int(a[k]))/(end-start), 3) if end > start else None
                           for k in ['rx_bytes', 'tx_bytes', 'notify']}}
    counts = {}
    for key, samples in counters.items():
        values = [v for _, v in samples]
        counts[key] = {'first': values[0], 'last': values[-1], 'delta': values[-1]-values[0],
                       'min': min(values), 'max': max(values),
                       'counter_regressions': sum(b < a for a, b in zip(values, values[1:]))}
    coverage = {}
    for device in manifest.get('devices', {}):
        times = [r['monotonic'] for r in rows if r['device'] == device]
        if times:
            coverage[device] = {'first_delay_s': round(times[0]-lo, 3),
                                'last_delay_s': round((hi or times[-1])-times[-1], 3),
                                'max_gap_s': round(max((b-a for a, b in zip(times, times[1:])), default=0), 3)}
        else: coverage[device] = None
    return {'complete': hi is not None, 'duration_s': round((hi or rows[-1]['monotonic'])-lo, 3) if rows else 0,
            'requested_s': manifest['duration_s'], 'rows': len(rows), 'reboots_or_panics': reboots,
            'signals': rates, 'rate_policies': policy_report, 'rate_policy_reapplications':manifest.get('rate_policy_reapplications',[]), 'peer_traffic': traffic, 'counters': counts,
            'ranges': {k: summarize(v) for k, v in series.items()}, 'malformed_rows': malformed,
            'capture_coverage': coverage,
            'partial_jsonl_rows': partial,
            'limits': 'DEMO with physical CAN TX disabled. USB receive times; malformed rows excluded. Does not qualify vehicle CAN, optical/touch behavior, or iPad.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = analyze(args.capture)
    text = json.dumps(result, indent=2)
    if args.output: args.output.write_text(text+'\n')
    print(text)
