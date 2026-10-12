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

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--ipad-db',required=True);p.add_argument('--drive-log',required=True);p.add_argument('--output',required=True)
    a=p.parse_args();print(json.dumps(historical(a.ipad_db,a.drive_log,a.output),indent=2))
if __name__=='__main__':main()
