#!/usr/bin/env python3
"""PID S1 (PM Claude 2026-11-03): read-only counter tests, mutation proofs and
fixed-base exact TX/RX trace equivalence. Host only: no devices, no CAN.

1. tests/pid_s1.cpp (independent expected counter values) under the default
   RP8 owner and under the effective owner_live env flags.
2. Mutations in a temporary copy of src/include/tests only: diagnostic header,
   owner hooks and owner shed/STOP/retry behaviour. A compile failure is an
   error, never a kill.
3. Existing owner scenarios: TX/RX/tick trace of base 3e43dfe, head, head with
   counters forced to UINT64_MAX before every tick, and head with counters
   zeroed and diagnostic state scrambled before every tick must be identical.
4. Existing production monitor scenarios: driver-boundary TX (incl. FC) and
   all RX identical to base.
"""
import hashlib, os, pathlib, re, shutil, subprocess, sys, tempfile
from test_pid_s1_scope import ROOT, BASE, check
sys.path.insert(0,str(ROOT/'tests'))
from test_signal_v2_b01_policy import flags_by_env, OWNER, HIL
CXX=os.environ.get('CXX','c++')
WARN=['-std=c++17','-Wall','-Wextra','-Werror']
V2=['-DM5CAN_SIGNAL_V2_CAN_EXPERIMENTAL=1']
RP8=['-DM5CAN_SIGNAL_V2_RP8_PROFILE=1']
ECU=['-DM5CAN_ECU_SELECTION_EXPERIMENTAL=1']
ADAPT=['-DM5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL=1']
CORE=['src/diagnostic_policy.cpp','src/diagnostic_transaction.cpp','src/isotp_reassembler.cpp']
HOST=['tests/host_stubs','include','src']
PR65=['tests/pr65/stubs']+HOST
HILS=['tests/hil/stubs']+HOST
_,EFFECTIVE=flags_by_env(ROOT/'platformio.ini')
def env_defs(env):return [f for f in EFFECTIVE[env] if f.startswith(('-D','-U'))]
OWNER_LIVE=env_defs(OWNER)
assert '-DM5CAN_SIGNAL_V2_OWNER_CONTINUOUS_LIVE=1' in OWNER_LIVE and '-DM5CAN_SIGNAL_V2_OWNER_AUTO_ARM=1' in OWNER_LIVE

def run(cmd,**kw):
    p=subprocess.run(cmd,text=True,capture_output=True,**kw)
    assert p.returncode==0,(cmd,p.stdout[-3000:],p.stderr[-3000:])
    return p.stdout

def build(root,source,exe,flags,includes,extra=()):
    run([CXX,*WARN,*flags,*['-I'+str(root/p) for p in includes],str(source),
         *[str(root/p) for p in CORE+list(extra)],'-o',str(exe)])
    return exe

PID_S1_CONFIGS=[('rp8',V2+RP8,PR65),('owner_live',OWNER_LIVE,['tests/pr65/stubs']+HILS)]

# Every received/transmitted field is printed explicitly (never padding).
TRACE='''
static void traceOwnerTick(signal_v2::CanOwner& owner,uint32_t now,const signal_v2::Simulator::Demand& d,bool armed,uint32_t epoch=0){
 std::printf("TICK %u %u %u",now,unsigned(armed),epoch);for(auto p:d){std::printf(" %u",unsigned(p));}std::puts("");owner.tick(now,d,armed,epoch);
}
static void traceRequest(const m5can::DiagnosticRequest& q){
 std::printf("TX %u %08x %u %u %u %u %u",g_test_millis,unsigned(q.can_id),unsigned(q.length),unsigned(q.timeout_ms),unsigned(q.authorization_epoch),unsigned(q.signal_v2),unsigned(q.signal_v2_essential));
 for(unsigned i=0;i<q.length;++i){std::printf(" %02x",unsigned(q.data[i]));}std::puts("");
}
static void traceResult(const m5can::DiagnosticResult& r){
 std::printf("RX %u %u %llu %u %u",g_test_millis,unsigned(r.status),(unsigned long long)r.completed_at_us,unsigned(r.response_count),unsigned(r.raw_frame_count));
 for(unsigned i=0;i<r.response_count;++i){const auto& m=r.responses[i];std::printf(" %08x/%u",unsigned(m.can_id),unsigned(m.length));for(unsigned j=0;j<m.length;++j)std::printf("/%02x",unsigned(m.data[j]));}
 for(unsigned i=0;i<r.raw_frame_count;++i){const auto& m=r.raw_frames[i];std::printf(" raw:%08x/%u/%u",unsigned(m.can_id),unsigned(m.dlc),unsigned(r.raw_rx_offsets_us[i]));for(unsigned j=0;j<m.dlc;++j)std::printf("/%02x",unsigned(m.data[j]));}std::puts("");
}
'''
def fixture(source):
    s=source.read_text()
    s=re.sub(r'(\b\w+)\.tick\(([^;]+)\);',r'traceOwnerTick(\1,\2);',s)
    pos=s.index('namespace m5can {');s='#include <cstdio>\n'+s[:pos]+TRACE+s[pos:]
    s,n=re.subn(r'(bool CanMonitor::submitQuery\(const DiagnosticRequest\s*&\s*(\w+)\)\s*\{)',r'\1traceRequest(\2);',s)
    assert n==1,(source,'submitQuery stub')
    start=s.index('bool CanMonitor::pollQueryResult(');at=s.index('r=response;',start)
    s=s[:at]+'r=response;traceResult(r);'+s[at+len('r=response;'):]
    return s

# Existing owner scenarios with their run_all/b01 configuration.
SCENARIOS=[
 ('tests/test_signal_v2_owner_live.cpp',OWNER_LIVE,HILS),
 ('tests/hil/arm_group_fresh.cpp',env_defs(HIL),HILS),
 ('tests/pr65/owner-cadence.cpp',V2+ECU+ADAPT+RP8,PR65),
 ('tests/pr65/owner-cadence.cpp',V2+ECU+ADAPT+RP8+['-DM5CAN_SIGNAL_V2_10HZ_LAB=1'],PR65),
 ('tests/pr65/owner.cpp',V2+ECU+ADAPT,PR65),
 ('tests/pr65/owner.cpp',V2+ECU+ADAPT+RP8,PR65),
 ('tests/pr65/owner.cpp',V2+ECU+ADAPT+RP8+['-DM5CAN_SIGNAL_V2_10HZ_LAB=1'],PR65),
 ('tests/test_signal_v2_bounded_discovery.cpp',V2+ECU+RP8,HOST),
 ('tests/test_signal_v2_stage_s1.cpp',V2+RP8,HOST),
 ('tests/test_signal_v2_stage_s1.cpp',V2+RP8+['-DM5CAN_SIGNAL_V2_STAGE_GATED=1'],HOST),
 ('tests/test_signal_v2_review_owner.cpp',V2+ECU,HOST),
]
ANCHOR='    if(!can_)return;\n// P5_BEGIN'
FORCE={
 'extreme-max':'    for(auto &c:pid_diagnostics_.pid){c.demand=c.admitted=c.completed=c.shed=c.denied=c.failed=c.cancelled=UINT64_MAX;}\n'
               '    pid_diagnostics_.active_ms=UINT64_MAX;\n',
 'scrambled':'    {auto &z=pid_diagnostics_;for(auto &c:z.pid){c=PidDiagnostics::Count{};}z.active_ms=0;\n'
             '     z.pending=!z.pending;z.essential=!z.essential;z.active=!z.active;\n'
             '     z.due=now+7919U;z.last=now-104729U;z.epoch=~z.epoch;}\n',
}

def copy_tree(dst):
    for folder in ('src','include','tests'):shutil.copytree(ROOT/folder,dst/folder)
    return dst

OWNER_H='src/signal_v2_can_owner.h'
DIAG_H='include/signal_v2_pid_diagnostics.h'
MUTATIONS=[
 # Diagnostic header.
 ('completion removed',DIAG_H,'if(valid)add(pid[i].completed);','if(valid){}'),
 ('essential counts all PIDs',DIAG_H,'i<(essential?2U:5U);++i){','i<5U;++i){'),
 ('shed removed',DIAG_H,'add(pid[i].shed);','(void)pid[i].shed;'),
 ('busy demand lost',DIAG_H,'add(c.demand,n);','add(c.demand,1);'),
 ('cancel removed',DIAG_H,'add(pid[i].cancelled);','(void)pid[i].cancelled;'),
 ('demand only after admission',DIAG_H,'if(on&&int32_t(now-due)>=0)','if(on&&pending&&int32_t(now-due)>=0)'),
 ('saturation wraps',DIAG_H,'v=UINT64_MAX-v<n?UINT64_MAX:v+n;','v+=n;'),
 # Owner hooks.
 ('owner shed hook removed',OWNER_H,'      pid_diagnostics_.shedAux();\n',''),
 ('owner completion hook removed',OWNER_H,'    pid_diagnostics_.finish(valid);\n',''),
 ('owner admission counts probes',OWNER_H,'if(!discovery)pid_diagnostics_.admit(essential_);','pid_diagnostics_.admit(essential_);'),
 ('owner STOP keeps pending',OWNER_H,'    pid_diagnostics_.cancel();\n    pid_diagnostics_.active=false;\n','    pid_diagnostics_.active=false;\n'),
 ('owner essential denial hook removed',OWNER_H,'    if(!std::strcmp(reason,"LOAD_BUDGET_ESSENTIAL"))pid_diagnostics_.deny(true);\n',''),
 ('owner admission denial hook removed',OWNER_H,'    if(!std::strcmp(reason,"QUERY_ADMISSION_FAILED")&&ecu_.fixed())pid_diagnostics_.deny(essential_);\n',''),
 ('demand counted after STOP',OWNER_H,'armed&&!stopped_&&!s1_,epoch_','armed&&!s1_,epoch_'),
 # Owner behaviour regressions that the counters/shed detection must expose.
 ('owner always sheds',OWNER_H,'if(!full_headroom || !budget_.reserve(','if(!discovery || !full_headroom || !budget_.reserve('),
 ('owner never sheds',OWNER_H,'if(discovery){stop("LOAD_BUDGET_DISCOVERY");return;}','{stop("LOAD_BUDGET_DISCOVERY");return;}'),
 ('owner retries uncertain admission',OWNER_H,'cancellation.\n      stop("QUERY_ADMISSION_FAILED",10);return;','cancellation.\n      return;'),
 ('owner TX after STOP',OWNER_H,'    if(stopped_)return;\n    const auto s=can_->stats();','    const auto s=can_->stats();'),
]

def main():
    check()
    with tempfile.TemporaryDirectory() as tmp:
        tmp=pathlib.Path(tmp)
        for name,flags,inc in PID_S1_CONFIGS:
            out=run([str(build(ROOT,ROOT/'tests/pid_s1.cpp',tmp/('counter-'+name),flags,inc))])
            assert 'PASS PID S1' in out,out
            print('PASS new S1 independent counter assertions',name)
        # Mutation copies only code, never .git/.pio or the live source.
        mutant=copy_tree(tmp/'mutant')
        for name,path,a,b in MUTATIONS:
            target=mutant/path;original=(ROOT/path).read_text()
            assert original.count(a)==1,('mutation anchor',name)
            target.write_text(original.replace(a,b,1))
            exe=build(mutant,ROOT/'tests/pid_s1.cpp',tmp/'mutation',V2+RP8,PR65)
            p=subprocess.run([str(exe)],capture_output=True)
            target.write_text(original)
            assert p.returncode!=0,('SURVIVED',name)
            print('PASS mutation detected:',name)
        base=tmp/'base';base.mkdir()
        if subprocess.run(['git','cat-file','-e',BASE+'^{commit}'],cwd=ROOT,capture_output=True).returncode:
            subprocess.run(['git','fetch','--no-tags','origin',BASE],cwd=ROOT,check=True)
        archive=subprocess.Popen(['git','archive',BASE],cwd=ROOT,stdout=subprocess.PIPE)
        subprocess.run(['tar','-xf','-','-C',str(base)],stdin=archive.stdout,check=True)
        archive.stdout.close();assert archive.wait()==0
        variants={'base':base,'head':ROOT}
        body=(ROOT/OWNER_H).read_text();assert body.count(ANCHOR)==1
        for label,snippet in FORCE.items():
            root=copy_tree(tmp/label)
            (root/OWNER_H).write_text(body.replace(ANCHOR,snippet+ANCHOR,1))
            variants[label]=root
        for i,(source,flags,inc) in enumerate(SCENARIOS):
            f=tmp/('fixture%d.cpp'%i);f.write_text(fixture(ROOT/source))
            values={}
            for label,root in variants.items():
                p=subprocess.run([str(build(root,f,tmp/('exe-'+label),flags,inc))],capture_output=True,text=True)
                values[label]=(p.returncode,p.stdout)
            ref=values['base']
            assert ref[0]==0,(source,ref)
            for label,value in values.items():assert value==ref,(source,flags,label)
            lines=ref[1].splitlines()
            tx=sum(x.startswith('TX ') for x in lines);rx=sum(x.startswith('RX ') for x in lines)
            assert any(x.startswith('TICK ') for x in lines),source
            # The empty built-in profile has no approved candidate: zero TX is its contract.
            assert tx>0 if not (source=='tests/pr65/owner.cpp' and RP8[0] not in flags) else tx==0,(source,flags,tx)
            print('PASS base == head == extreme-max == scrambled TX/RX trace',source,
                  ' '.join(x for x in flags if 'RP8' in x or '10HZ' in x or 'STAGE' in x or 'OWNER' in x or 'HIL' in x) or 'no-RP8-flag',
                  'tx=%d rx=%d'%(tx,rx),
                  hashlib.sha256(ref[1].encode()).hexdigest()[:16])
        # Positive control: a one-field TX change (deadline +1ms) must break the
        # trace comparison, so equality above is not vacuous.
        control=copy_tree(tmp/'tx-control');t='req.timeout_ms=profiles::kLegacyRp8.deadline_ms;\n    essential_=false;'
        assert body.count(t)==1
        (control/OWNER_H).write_text(body.replace(t,'req.timeout_ms=profiles::kLegacyRp8.deadline_ms+1;\n    essential_=false;',1))
        source,flags,inc=SCENARIOS[2];f=tmp/'control.cpp';f.write_text(fixture(ROOT/source))
        outs=[subprocess.run([str(build(r,f,tmp/('control-'+n),flags,inc))],capture_output=True,text=True).stdout for n,r in (('base',base),('mut',control))]
        assert outs[0]!=outs[1]
        print('PASS positive control: deadline+1ms TX mutation changes the trace')
        # Existing production-monitor scenarios: rolling boundaries, full
        # 120ms, FC, all-RX charging, RX loss, retry/STOP and lower gate.
        src=(ROOT/'tests/pr65/monitor.cpp').read_text()
        a='esp_err_t twai_transmit(const twai_message_t* m,TickType_t) {'
        b='*m=rx.front().frame;'
        assert src.count(a)==1 and src.count(b)==1
        src=src.replace(a,a+'\n std::printf("WIRE_TX %u %08x %u",g_test_millis,unsigned(m->identifier),unsigned(m->data_length_code));for(unsigned i=0;i<m->data_length_code;++i){std::printf(" %02x",unsigned(m->data[i]));}std::puts("");')
        src=src.replace(b,b+'std::printf("WIRE_RX %u %08x %u",g_test_millis,unsigned(m->identifier),unsigned(m->data_length_code));for(unsigned j=0;j<m->data_length_code;++j){std::printf(" %02x",unsigned(m->data[j]));}std::puts("");')
        f=tmp/'monitor.cpp';f.write_text('#include <cstdio>\n'+src)
        values=[]
        for label in ('base','head','extreme-max'):
            exe=build(variants[label],f,tmp/('monitor-'+label),V2+ECU+ADAPT+RP8,PR65,['src/can_monitor.cpp','tests/host_stubs/stubs.cpp'])
            values.append(''.join(run([str(exe),case]) for case in ('M1','M2','M3','M6','M7','m1','controls','R1','R2','R3')))
        assert values[0]==values[1]==values[2] and 'WIRE_TX ' in values[0] and 'WIRE_RX ' in values[0]
        fc=sum(1 for line in values[0].splitlines() if line.startswith('WIRE_TX ') and line.split()[4]=='30')
        assert fc>0
        print('PASS fixed-base production monitor wire trace (FC=%d)'%fc,hashlib.sha256(values[0].encode()).hexdigest()[:16])
if __name__=='__main__':main()
