#!/usr/bin/env python3
"""Sweep production tick/monitor IO boundaries with actual GATT revocation."""
import pathlib,sys,subprocess,os,tempfile
HARNESS=pathlib.Path(__file__).resolve().parent
sys.path.insert(0,str(HARNESS))
from test_signal_v2_b01_policy import flags_by_env,OWNER
root=pathlib.Path(os.environ.get('PR70_SOURCE_ROOT',HARNESS.parent)).resolve()
tmp=tempfile.TemporaryDirectory();out=pathlib.Path(tmp.name)
s=(root/'src/signal_v2_gatt.cpp').read_text()
def block(a,b):return a+s.split(a,1)[1].split(b,1)[0]
parts={
'@GRANT@':block('bool grantCanArm(uint32_t now,uint32_t seen) {','\n#endif\nvoid manage('),
'@STOP@':block('  if(!std::strcmp(cmd,"SIGV2 CAN STOP")) {','\n#if defined(M5CAN_SIGNAL_V2_SERIAL_HIL)\n  if(!std::strcmp(cmd,"SIGV2 CAN READY"))'),
'@ARM@':block('  if(!std::strcmp(cmd,"SIGV2 CAN ARM")) {','  // PR-C2 stage S1'),
'@DISCONNECT@':block('void disconnected(uint16_t handle) {','void gap('),
'@TTL@':block('  if(can_armed && int32_t(armed_until-now)<=0){','  owner.tick(now,demand,can_armed,'),
'@AUTO@':block('void manualCanStop() {','\n#endif\nbool usbCommand(')}
u=(root/'src/ui.cpp').read_text();a='bool Ui::pollDemoToggle() {'
parts['@TOUCH@']=a+u.split(a,1)[1].split('\nvoid Ui::update(',1)[0]
f=(HARNESS/'owner_auto_arm_fixture.cpp.in').read_text()
for k,v in parts.items(): f=f.replace(k,v)


f=f.replace('static m5can::CanStats health{};','')
a=f.index('namespace m5can {');b=f.index('\nCanOwner owner;',a)
f=f[:a]+(HARNESS/'pr70/runtime.inc').read_text()+f[b:]
f=f[:f.index('int main(')]+(HARNESS/'pr70/main.inc').read_text()

p=out/'race.cpp';p.write_text(f)
_,flags=flags_by_env(root/'platformio.ini')
for profile in ('owner', 'shared', 'staged', '10hz'):
    defs=[x for x in flags[OWNER] if x.startswith(('-D','-U'))]
    if profile!='owner':
        defs=[x for x in defs if 'OWNER_AUTO_ARM' not in x and 'OWNER_CONTINUOUS_LIVE' not in x]
        # GATT tap/auto-session bodies are owner-only; keep them in this IO fixture.
        # The actual monitor/CanOwner flags below are compiled separately.
    obj=out/(profile+'.o')
    monitor_defs=list(defs)
    if profile in ('staged','10hz'):monitor_defs+=['-DM5CAN_SIGNAL_V2_STAGE_GATED=1']
    if profile=='10hz':monitor_defs+=['-DM5CAN_SIGNAL_V2_10HZ_LAB=1']
    includes=['-I'+str(HARNESS/'pr70/stubs'),'-Itests/pr65/stubs','-Itests/host_stubs','-Iinclude','-Isrc']
    base=['c++','-std=c++17','-Wall','-Wextra','-Werror','-DM5CAN_TEST_S1_STOP_RACE=1',*includes]
    subprocess.run([*base,*monitor_defs,'-c','src/can_monitor.cpp','-o',str(obj)],cwd=root,check=True)
    # Layout-affecting owner flags must match CanOwner; GATT fixture auto session
    # is retained as the producer of the production disconnect/tap callbacks.
    fixture_defs=list(monitor_defs)
    fixture_defs+=['-DM5CAN_SIGNAL_V2_OWNER_CONTINUOUS_LIVE=1','-DM5CAN_SIGNAL_V2_OWNER_AUTO_ARM=1'] if profile!='owner' else []
    if 'authorization_epoch = 0' in (root/'src/can_monitor.h').read_text():fixture_defs+=['-DPR70_EPOCH_API=1']
    exe=out/profile
    subprocess.run([*base,*fixture_defs,str(p),str(obj),'src/diagnostic_policy.cpp',
      'src/diagnostic_transaction.cpp','src/isotp_reassembler.cpp','src/signal_v2_session.cpp',
      'src/signal_v2_codec.cpp','src/signal_v2_can_samples.cpp','src/signal_v2_discovery_profile.cpp',
      '-o',str(exe)],cwd=root,check=True)
    positive=subprocess.run([str(exe)],capture_output=True,text=True,check=True)
    count=int(positive.stdout.split('POINTS ')[1].split()[0])
    failures=[]
    for point in range(1,count+1):
        for mode in ([],['tap']):
            r=subprocess.run([str(exe),str(point),*mode],capture_output=True,text=True)
            if r.returncode:failures.append((point,'tap' if mode else 'disconnect',r.stderr.strip()))
    print(profile, 'positive current-epoch TX=1;',count*2,'interleavings;', 'failures=',len(failures),flush=True)
    if failures:
        for failure in failures[:5]:print(failure,flush=True)
        raise SystemExit(1)
