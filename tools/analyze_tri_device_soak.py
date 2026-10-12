#!/usr/bin/env python3
"""Offline SQLite/USB analysis. Never equate adapter boot milliseconds to UTC."""
import argparse,csv,datetime,json,re,sqlite3
from pathlib import Path

def stamp(s):return datetime.datetime.fromisoformat(s.replace('Z','+00:00')).timestamp()

def historical(database, drive_log, output):
    connection=sqlite3.connect('file:'+str(Path(database).resolve())+'?mode=ro',uri=True)
    integrity=connection.execute('pragma integrity_check').fetchone()[0]
    events=connection.execute("select ts_utc,kind from events where note like '%BLE接続先が変更%' order by ts_utc").fetchall()
    text=Path(drive_log).read_text()
    groups={}
    for m in re.finditer(r'#DRIVELOG,EVENT,boot=(\d+),ms=(\d+),kind=4,peer=(\d+)',text):
        groups.setdefault((int(m[1]),int(m[3])),[]).append(int(m[2])/1000)
    if not events:raise ValueError('No adapterChanged events to align')
    # First-event anchoring is explicit. Evaluate all boot/peer candidates;
    # matching residuals support correspondence, not independent clock sync.
    fits=[]
    for (boot,peer),times in groups.items():
        if len(times)<len(events):continue
        offset=stamp(events[0][0])-times[0]
        pairs=[]
        for utc,kind in events:
            t=stamp(utc);near=min(times,key=lambda a:abs(t-offset-a))
            pairs.append({'ipad_utc':utc,'adapter_boot':boot,'peer':peer,'adapter_ms':round(near*1000),'residual_s':round(t-offset-near,3)})
        if len({r['adapter_ms'] for r in pairs})<len(pairs):continue
        fits.append({'boot':boot,'peer':peer,'max_residual_s':max(abs(r['residual_s']) for r in pairs),'pairs':pairs})
    fits.sort(key=lambda x:x['max_residual_s'])
    result={'integrity':integrity,'method':'first-event offset fit, not independently synchronized clocks','fits':fits,'missing':'old captures lack CoreBluetooth/GAP reason codes; cannot infer initiating side'}
    destination=Path(output);destination.mkdir(parents=True,exist_ok=True)
    (destination/'historical-analysis.json').write_text(json.dumps(result,indent=2))
    if fits:
        with (destination/'historical-aligned.csv').open('w') as f:
            w=csv.DictWriter(f,fieldnames=list(fits[0]['pairs'][0]));w.writeheader();w.writerows(fits[0]['pairs'])
    return result

def run_analysis(capture, ipad_jsonl, database=None):
    root=Path(capture)
    app=[json.loads(line) for line in Path(ipad_jsonl).read_text().splitlines()]
    host=[json.loads(line) for line in (root/'mac_events.jsonl').read_text().splitlines()]
    start=next((r for r in app if r['kind']=='DEMO_VERIFIED'),None)
    end=next((r for r in reversed(app) if r['kind']=='SOAK_END'),None)
    samples=[r for r in app if r['kind']=='SAMPLE']
    failures=[r for r in app if r['kind']=='SOAK_FAILURE']
    lo=stamp(start['utc']) if start else float('inf')
    hi=stamp(end['utc']) if end else float('inf')
    selected=[r for r in host if lo<=stamp(r['utc'])<=hi]
    fields=lambda line:dict(re.findall(r'([A-Za-z0-9_]+)=([^,\s]+)',line))
    disconnects=[];reboots=[];signal_rows={};peer_start={};peer_end={};malformed_signals=0;malformed_peers=0
    for r in selected:
        line=r['line'];d=fields(line)
        if '#BLE_EVENT,' in line and d.get('kind')=='DISCONNECT':
            disconnects.append({'utc':r['utc'],**d})
        if 'ESP-ROM:' in line:reboots.append({'utc':r['utc'],'device':r['device']})
        if '#BLE_PEER,' in line:
            key=(d.get('id'),d.get('gen'))
            if d.get('up')=='1' and {'rx_bytes','tx_bytes'}<=d.keys() and all(d[k].isdigit() for k in ['rx_bytes','tx_bytes']):
                peer_start.setdefault(key,(r,d));peer_end[key]=(r,d)
        if 'M5CAN SIGNAL' in line:
            if not {'id','updates','age_ms','quality'}<=d.keys() or not all(d[k].isdigit() for k in ['id','updates','age_ms','quality']):
                malformed_signals+=1;continue
            signal_rows.setdefault(r['device']+':'+d['id'],[]).append((r,d))
    traffic=[]
    for key,(last,d) in peer_end.items():
        first,a=peer_start[key];seconds=stamp(last['utc'])-stamp(first['utc'])
        if seconds>0:traffic.append({'connection_id':key[0],'generation':key[1],'sample_s':round(seconds,3),'rx_Bps':round((int(d['rx_bytes'])-int(a['rx_bytes']))/seconds,3),'tx_Bps':round((int(d['tx_bytes'])-int(a['tx_bytes']))/seconds,3)})
    rates={}
    for name,rows in signal_rows.items():
        first,a=rows[0];last,b=rows[-1];seconds=stamp(last['utc'])-stamp(first['utc'])
        delta=int(b['updates'])-int(a['updates'])
        rates[name]={'observations':len(rows),'sample_s':round(seconds,3),'updates_delta':delta,'actual_hz':round(delta/seconds,3) if seconds>0 and delta>=0 else None,'max_age_ms':max(int(d['age_ms']) for _,d in rows),'last_age_ms':int(b['age_ms']),'last_quality':int(b['quality'])}
    latencies=[]
    for r in samples:
        m=re.search(r'latency_ms=([0-9.]+)',r['note'])
        if m:latencies.append(float(m[1]))
    latencies.sort()
    percentile=lambda q:latencies[min(len(latencies)-1,int((len(latencies)-1)*q))] if latencies else None
    result={'app_complete':bool(end),'app_failures':failures,'samples':len(samples),'expected_error_events':sum(r['kind']=='EXPECTED_ERROR' for r in app),'duration_from_demo_s':round(hi-lo,3) if start and end else None,'latency_ms':{'p50':percentile(.5),'p95':percentile(.95),'p99':percentile(.99),'max':max(latencies) if latencies else None},'disconnects_during_app':disconnects,'reboots_during_app':reboots,'signals':rates,'peer_traffic':traffic,'malformed_signal_rows':malformed_signals,'missing':'Capture coverage and all-peer mapping must be checked before physical PASS; receipt timestamps are not exact controller callback clocks.'}
    if database:
        c=sqlite3.connect('file:'+str(Path(database).resolve())+'?mode=ro',uri=True)
        result['sqlite_integrity']=c.execute('pragma integrity_check').fetchone()[0]
        result['command_counts']=c.execute('select count(*),sum(success) from commands').fetchone()
        result['session_states']=c.execute('select status,git_commit,tool_version from sessions').fetchall()
    if host:
        result['host_coverage_utc']=[host[0]['utc'],host[-1]['utc']]
        result['full_host_coverage']=bool(start and end and stamp(host[0]['utc'])<=lo and stamp(host[-1]['utc'])>=hi)
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ipad-db');p.add_argument('--drive-log');p.add_argument('--output',required=True)
    p.add_argument('--capture');p.add_argument('--ipad-jsonl')
    a=p.parse_args()
    if a.capture and a.ipad_jsonl:
        result=run_analysis(a.capture,a.ipad_jsonl,a.ipad_db)
        out=Path(a.output);out.mkdir(parents=True,exist_ok=True)
        (out/'run-analysis.json').write_text(json.dumps(result,indent=2))
    elif a.ipad_db and a.drive_log:result=historical(a.ipad_db,a.drive_log,a.output)
    else:p.error('use --capture/--ipad-jsonl or --ipad-db/--drive-log')
    print(json.dumps(result,indent=2))
if __name__=='__main__':main()
