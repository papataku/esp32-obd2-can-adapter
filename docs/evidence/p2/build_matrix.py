"""Build-only synthetic-fixture matrix; never enumerates or accesses devices."""
import configparser, concurrent.futures, os, pathlib, subprocess, sys
root=pathlib.Path.cwd()
sys.path.insert(0,str(root/'tools'))
from vehicle_trial_profile import ARCHIVED_INSTALLED_PARTITIONS_SHA256, APP_ONLY_ACK
p=configparser.ConfigParser(interpolation=None);p.read('platformio.ini')
names=[s[4:] for s in p.sections() if s.startswith('env:')]+['smartring_signal_v2_client_lab']
output=root.parent/'evidence';output.mkdir(exist_ok=True)
def build(name):
    env=os.environ.copy()
    env['M5CAN_LOG_V2_ALLOWED_PEER_ID48']=format((2<<40)+1,'#014x')
    if name.startswith('m5dial_v2_fresh_flash'):
        env['M5CAN_V2_FRESH_FLASH_ACK']='ERASE_ALL_8MB_OLD_DATA_REQUIRED'
    elif name=='m5dial_existing_singleapp_vehicle_trial':
        env.pop('M5CAN_V2_FRESH_FLASH_ACK',None)
        env['M5CAN_VEHICLE_TRIAL_APPROVED_PEER_ID48']=format((2<<40)+1,'#014x')
        env['M5CAN_VEHICLE_TRIAL_INSTALLED_PARTITIONS_SHA256']=ARCHIVED_INSTALLED_PARTITIONS_SHA256
        env['M5CAN_VEHICLE_TRIAL_APP_ONLY_ACK']=APP_ONLY_ACK
    args=['-c','platformio_smartring_v2_lab.ini'] if name=='smartring_signal_v2_client_lab' else ['-e',name]
    with (output/('matrix-'+name+'.log')).open('w') as log:
        result=subprocess.run([os.path.expanduser('~/.platformio/penv/bin/pio'),'run',*args,'-j','4'],env=env,stdout=log,stderr=subprocess.STDOUT)
    return name,result.returncode
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
    jobs=[pool.submit(build,name) for name in names]
    results=[]
    for job in concurrent.futures.as_completed(jobs):
        name,code=job.result();results.append((name,code));print(name,code,flush=True)
print('TOTAL',len(results),'PASS',sum(code==0 for _,code in results),flush=True)
raise SystemExit(any(code for _,code in results))
