#!/usr/bin/env python3
"""Bounded USB capture for Issue21/95. App launch/SQLite recovery are explicit.

No arbitrary CAN/ELM commands are supported. --demo-auto requests the existing
USB DEMO AUTO setting and verifies active=YES before the measurement starts.
"""
import argparse, csv, datetime, json, re, subprocess, threading, time
from pathlib import Path


def fields(line):
    return dict(re.findall(r'([A-Za-z0-9_]+)=([^,\s]+)', line))


def bench_applied(line, preset):
    expected = {'moderate': ('10', '5', '5'), 'high': ('20', '20', '20')}[preset]
    d = fields(line)
    return line.startswith('M5CAN BENCH RATE APPLIED ') and tuple(d.get(k) for k in ['rpm', 'speed', 'power']) == expected and d.get('coolant') == d.get('soc') == '1'


def capture(args):
    import serial
    from serial.tools import list_ports
    output=Path(args.output);output.mkdir(parents=True,exist_ok=False)
    expected={'adapter':'48:CA:43:3A:11:F4','ring':'10:51:DB:8E:DD:40'}
    ports={p.device:p for p in list_ports.comports()}
    mappings={'adapter':args.adapter,'ring':args.ring}
    if args.jc:mappings['jc']=args.jc;expected['jc']='98:3D:AE:E9:FB:18'
    if len(set(mappings.values()))!=len(mappings):raise RuntimeError('ports must be distinct')
    manifest={'start_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'scenario':args.scenario,'duration_s':args.seconds,'devices':{},'clock':'host UTC+monotonic; device uptime separately, initial resets excluded','fw_sha':args.fw_sha,'app_sha':args.app_sha,'meter_sha':args.meter_sha}
    for name,port in mappings.items():
        p=ports.get(port)
        if not p or p.serial_number!=expected[name] or (p.vid,p.pid)!=(0x303a,0x1001):raise RuntimeError('USB identity mismatch: '+name)
        manifest['devices'][name]={'port':port,'serial':p.serial_number,'vid':p.vid,'pid':p.pid}
    if args.ipad_device:
        if not re.fullmatch(r'[A-Za-z0-9_-]{1,80}',output.name):raise RuntimeError('unsafe app run ID')
        if args.seconds < args.ipad_seconds + 30:raise RuntimeError('capture must exceed iPad duration by at least30s')
        manifest['devices']['ipad']={'identifier':args.ipad_device,'bundle':'com.papataku.HondaAnalyzer'}
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2))
    stop=threading.Event();lock=threading.Lock();latest={};rows=[];links={};errors=[]
    events=(output/'mac_events.jsonl').open('w',buffering=1)
    def read(name,link):
        raw=(output/(name+'_serial.log')).open('wb',buffering=0);buf=b''
        try:
            while not stop.is_set():
                chunk=link.read(16384)
                if not chunk:continue
                raw.write(chunk);buf+=chunk
                while b'\n' in buf:
                    line,buf=buf.split(b'\n',1);text=line.decode(errors='replace').strip()
                    row={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'monotonic':time.monotonic(),'device':name,'line':text}
                    with lock:
                        events.write(json.dumps(row)+'\n');rows.append(row)
                        if '#DEMO,' in text:latest['demo']=fields(text)
                        if text.startswith('#STATS,'):latest['stats']=fields(text)
                        if text.startswith('#DRIVELOG,boot='):latest['trip']=fields(text)
                        if '#DRIVELOG,END' in text:latest['trip_complete']=True
                        if text.startswith('#BLE_DUAL,'):latest['dual']=fields(text)
                        if text.startswith('#BLELOG,END'):latest['blelog_complete']=True
                        if text.startswith('M5CAN BENCH RATE APPLIED '):latest[name+'_bench']=text
        except Exception as exc:
            with lock:errors.append(name+': '+repr(exc))
        finally:raw.close()
    threads=[]
    try:
        for name,port in mappings.items():
            link=serial.Serial();link.port=port;link.baudrate=115200;link.timeout=.1;link.dtr=False;link.rts=False;link.open();links[name]=link
            t=threading.Thread(target=read,args=(name,link),daemon=True);t.start();threads.append(t)
        def send(name,cmd):
            links[name].write(cmd.encode()+b'\n');links[name].flush()
            with lock:events.write(json.dumps({'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'monotonic':time.monotonic(),'device':'host','line':name+' COMMAND '+cmd})+'\n')
        time.sleep(4)
        if args.demo_auto:
            send('adapter','DEMO AUTO');deadline=time.monotonic()+5
            while time.monotonic()<deadline and latest.get('demo',{}).get('active')!='YES':time.sleep(.1)
        if latest.get('demo',{}).get('active')!='YES':
            # Unchanged settings are silent; live STATS explicitly reports
            # the current DEMO mode without a blocking journal export.
            send('adapter','STATS');deadline=time.monotonic()+8
            while time.monotonic()<deadline and latest.get('stats',{}).get('demo')!='ON':time.sleep(.1)
            if latest.get('stats',{}).get('demo')!='ON':
                raise RuntimeError('DEMO not confirmed: measurement prohibited')
        # SmartRing defers runtime init ~6s: never silently drop isolation commands.
        time.sleep(8)
        if args.isolate_jc and 'jc' in links:send('jc','x')
        if args.isolate_ring or args.ring_after_ipad:send('ring','x')
        time.sleep(2)
        if args.meter_order:
            if not args.require_two_meters or 'jc' not in links or args.isolate_ring or args.isolate_jc or args.ipad_device:
                raise RuntimeError('meter order requires two meters without iPad/isolation')
            send('ring','x');send('jc','x')
            deadline=time.monotonic()+15
            while time.monotonic()<deadline and latest.get('dual',{}).get('clients')!='0':time.sleep(.1)
            if latest.get('dual',{}).get('clients')!='0':raise RuntimeError('both meters must disconnect before ordered connect')
            first,second=('ring','jc') if args.meter_order=='ring-first' else ('jc','ring')
            send(first,'c');deadline=time.monotonic()+20
            while time.monotonic()<deadline and latest.get('dual',{}).get('clients')!='1':time.sleep(.1)
            if latest.get('dual',{}).get('clients')!='1':raise RuntimeError('first actual peer not confirmed')
            # Allow ELM setup before the second peer; c is never sent while up.
            time.sleep(3);send(second,'c');deadline=time.monotonic()+20
            while time.monotonic()<deadline and latest.get('dual',{}).get('clients')!='2':time.sleep(.1)
            if latest.get('dual',{}).get('clients')!='2':raise RuntimeError('second actual peer not confirmed')
            time.sleep(8)
            manifest['meter_order']=args.meter_order
        if args.meter_bench_preset:
            # DEMO has already been verified. Only the volatile existing
            # single-byte meter commands are allowed; never change NVS rates.
            if not args.require_two_meters or 'jc' not in links or args.isolate_ring or args.isolate_jc:
                raise RuntimeError('bench preset requires both active meters')
            command = '#' if args.meter_bench_preset == 'high' else '!'
            send('ring', command);send('jc', command)
            deadline=time.monotonic()+10
            while time.monotonic()<deadline:
                if all(bench_applied(latest.get(name+'_bench',''),args.meter_bench_preset) for name in ['ring','jc']):break
                time.sleep(.1)
            else:raise RuntimeError('volatile bench rates not applied on both meters')
            manifest['meter_bench_preset']=args.meter_bench_preset
            manifest['meter_bench_rates_volatile']=True
        started=time.monotonic();manifest['measurement_start_monotonic']=started
        (output/'manifest.json').write_text(json.dumps(manifest,indent=2))
        if args.ipad_device:
            launch=['xcrun','devicectl','device','process','launch','--device',args.ipad_device,
                    '--terminate-existing','com.papataku.HondaAnalyzer','--ble-soak',
                    '--soak-scenario',args.ipad_scenario,'--soak-seconds',str(args.ipad_seconds),
                    '--soak-delay-ms',str(args.ipad_delay_ms),'--soak-positive-repeats',str(args.ipad_positive_repeats),
                    '--soak-run-id',output.name,'--timeout','15','--json-output',str(output/'ipad-launch.json')]
            completed=subprocess.run(launch,capture_output=True,text=True,timeout=20)
            (output/'ipad-launch.log').write_text(completed.stdout+completed.stderr)
            if completed.returncode:raise RuntimeError('iPad launch blocked; inspect ipad-launch.log and request required unlock/approval')
        if args.ring_after_ipad:
            deadline=time.monotonic()+20
            confirmed=False
            while time.monotonic()<deadline:
                copy=['xcrun','devicectl','device','copy','from','--device',args.ipad_device,
                      '--domain-type','appDataContainer','--domain-identifier','com.papataku.HondaAnalyzer',
                      '--source','Documents/BLESoak/'+output.name+'.jsonl',
                      '--destination',str(output/'ipad-ready.jsonl'),'--timeout','5']
                got=subprocess.run(copy,capture_output=True,text=True,timeout=8)
                if got.returncode==0 and '"kind":"DEMO_VERIFIED"' in (output/'ipad-ready.jsonl').read_text().replace(' ',''):
                    confirmed=True;break
                time.sleep(1)
            if not confirmed:raise RuntimeError('iPad-first DEMO verification missing; do not reconnect Ring')
            # Startup restores exactly ONE saved verified M5CAN candidate.
            # c resumes it without an ambient scan/selection of another ELM.
            send('ring','c')
            manifest['order']='iPad DEMO verified, then SmartRing connect command'
            (output/'manifest.json').write_text(json.dumps(manifest,indent=2))
        if args.require_two_meters:
            if 'jc' not in links or args.isolate_jc:raise RuntimeError('two-meter gate requires active JC')
            send('adapter','STATS');send('ring','i');send('jc','i')
            deadline=time.monotonic()+20
            while time.monotonic()<deadline and latest.get('dual',{}).get('clients')!='2':time.sleep(.1)
            if latest.get('dual',{}).get('clients')!='2':raise RuntimeError('two actual BLE peers not confirmed')
            started=time.monotonic();manifest['measurement_start_monotonic']=started
            manifest['measurement_start_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat()
            (output/'manifest.json').write_text(json.dumps(manifest,indent=2))
        end=started+args.seconds;next_status=0
        while time.monotonic()<end:
            if errors:raise RuntimeError(str(errors))
            if time.monotonic()>=next_status:
                send('adapter','STATS');send('ring','i')
                if 'jc' in links and not args.isolate_jc:send('jc','i')
                next_status=time.monotonic()+10
            time.sleep(.2)
        manifest['measurement_end_monotonic']=time.monotonic()
        manifest['measurement_end_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat()
        (output/'manifest.json').write_text(json.dumps(manifest,indent=2))
        if args.export_evidence:
            # Stop both consumers before any synchronous post-test USB dump.
            send('ring','x')
            if 'jc' in links:send('jc','x')
            time.sleep(8) # bounded logger's urgent checkpoint floor
            for command in ['LOGCAP','BLELOG INDEX','BLELOG','DRIVELOG']:
                send('adapter',command);time.sleep(3)
    except BaseException as exc:
        errors.append(repr(exc))
    finally:
        stop.set()
        for thread in threads:thread.join(timeout=1)
        for link in links.values():link.close()
        events.close()
    if args.ipad_device:
        copy=['xcrun','devicectl','device','copy','from','--device',args.ipad_device,
              '--domain-type','appDataContainer','--domain-identifier','com.papataku.HondaAnalyzer',
              '--source','Documents/BLESoak','--destination',str(output/'ipad-bundle'),'--timeout','30']
        completed=subprocess.run(copy,capture_output=True,text=True,timeout=35)
        (output/'ipad-copy.log').write_text(completed.stdout+completed.stderr)
        if completed.returncode:errors.append('iPad evidence export failed')
    measured=[r for r in rows if manifest.get('measurement_start_monotonic',float('inf'))<=r['monotonic']<=manifest.get('measurement_end_monotonic',float('inf'))]
    metrics=analyze(measured);metrics['capture_errors']=errors
    (output/'metrics.json').write_text(json.dumps(metrics,indent=2))
    with (output/'aligned_timeline.csv').open('w') as f:
        writer=csv.DictWriter(f,fieldnames=['utc','monotonic','device','line']);writer.writeheader();writer.writerows(rows)
    (output/'report.md').write_text('# USB capture\n\n'+json.dumps(metrics,indent=2)+'\n\nUSB capture alone is not iPad test acceptance. Attach matching iPad SQLite and JSONL; never infer an unobserved peer.\n')
    print(json.dumps(metrics,indent=2),flush=True)
    if errors:raise SystemExit(1)


def analyze(rows):
    result={'serial_rows':len(rows),'disconnects':[],'reboots':[],'last_peers':{},'last_signals':{},'notify_failures':0,'dual_snapshots':[]}
    for row in rows:
        line=row['line'];d=fields(line)
        if '#BLE_EVENT,' in line and d.get('kind')=='DISCONNECT':result['disconnects'].append({'host_utc':row['utc'],**d})
        if 'ESP-ROM:' in line:result['reboots'].append({'device':row['device'],'host_utc':row['utc']})
        if '#BLE_PEER,' in line:result['last_peers'][d.get('slot','unknown')]=d
        if 'M5CAN SIGNAL ' in line:result['last_signals'][row['device']+':'+d.get('id','unknown')]=d
        if '#BLE_DUAL,' in line:
            result['notify_failures']=int(d.get('failed',0));result['dual_snapshots'].append({'utc':row['utc'],**d})
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['adapter','ring','output']:p.add_argument('--'+name,required=True)
    p.add_argument('--meter-sha',default='unknown')
    p.add_argument('--meter-bench-preset',choices=['moderate','high'])
    p.add_argument('--meter-order',choices=['ring-first','jc-first'])
    p.add_argument('--require-two-meters',action='store_true');p.add_argument('--export-evidence',action='store_true')
    p.add_argument('--ipad-delay-ms',type=float,default=200);p.add_argument('--ipad-positive-repeats',type=int,default=1)
    p.add_argument('--ipad-device');p.add_argument('--ipad-scenario',choices=['idle','single','batch','no-data','faults'],default='batch');p.add_argument('--ipad-seconds',type=float,default=1800)
    p.add_argument('--jc');p.add_argument('--seconds',type=float,default=60)
    p.add_argument('--scenario',default='smoke');p.add_argument('--fw-sha',default='unknown');p.add_argument('--app-sha',default='unknown')
    p.add_argument('--ring-after-ipad',action='store_true');p.add_argument('--demo-auto',action='store_true');p.add_argument('--isolate-jc',action='store_true');p.add_argument('--isolate-ring',action='store_true')
    a=p.parse_args()
    if not 1<=a.seconds<=10800:p.error('seconds must be in 1..10800')
    if not 1<=a.ipad_seconds<=3600:p.error('iPad duration must be in1..3600')
    if a.ring_after_ipad and not a.ipad_device:p.error('--ring-after-ipad requires --ipad-device')
    capture(a)
if __name__=='__main__':main()
