#!/usr/bin/env python3
"""S1 counter mutations and fixed-base exact TX/RX trace equivalence, no devices."""
import hashlib, os, pathlib, re, shutil, subprocess, tempfile
from test_pid_s1_scope import ROOT, BASE, check
CXX=os.environ.get('CXX','c++')
FLAGS=['-std=c++17','-Wall','-Wextra','-Werror','-DM5CAN_SIGNAL_V2_CAN_EXPERIMENTAL=1','-DM5CAN_SIGNAL_V2_RP8_PROFILE=1']
CORE=['src/diagnostic_policy.cpp','src/diagnostic_transaction.cpp','src/isotp_reassembler.cpp']
def run(cmd,**kw):
    p=subprocess.run(cmd,text=True,capture_output=True,**kw)
    assert p.returncode==0,(cmd,p.stdout,p.stderr)
    return p.stdout

def compile_run(root,source,exe,flags=(),monitor=False):
    includes=['tests/pr65/stubs','tests/host_stubs','include','src']
    sources=CORE+(['src/can_monitor.cpp','tests/host_stubs/stubs.cpp'] if monitor else [])
    run([CXX,*FLAGS,*flags,*['-I'+str(root/p) for p in includes],str(source),*[str(root/p) for p in sources],'-o',str(exe)])
    return ''.join(run([str(exe),case]) for case in ('M1','M2','M3','M6','M7','m1','controls','R1','R2','R3')) if monitor else run([str(exe)])

# Print fields, never object padding. Preserve times, request shape, result,
# every received RAW frame and FC at the stub driver boundary.
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
    s=re.sub(r'(\w+)\.tick\(([^;]+)\);',r'traceOwnerTick(\1,\2);',s)
    pos=s.index('namespace m5can {');s=s[:pos]+TRACE+s[pos:]
    s=s.replace('requests.push_back(', 'traceRequest(r);requests.push_back(') if 'requests.push_back(r)' in s else s.replace('requests.push_back(q)', 'traceRequest(q);requests.push_back(q)')
    s=s.replace('r=response;', 'r=response;traceResult(r);')
    return s

def main():
    check()
    with tempfile.TemporaryDirectory() as tmp:
        tmp=pathlib.Path(tmp)
        compile_run(ROOT,ROOT/'tests/pid_s1.cpp',tmp/'counter')
        print('PASS new S1 independent counter assertions')
        # Mutation copies only code, never .git/.pio or the live source.
        mutant=tmp/'mutant'
        for folder in ('src','include','tests'):shutil.copytree(ROOT/folder,mutant/folder)
        mutations=[('completion removed','if(valid)add(pid[i].completed);','if(valid){}'),
          ('essential counts all PIDs','i<(essential?2U:5U);++i){','i<5U;++i){'),
          ('shed removed','add(pid[i].shed);','(void)pid[i].shed;'),
          ('busy demand lost','add(c.demand,n);','add(c.demand,1);'),
          ('cancel removed','add(pid[i].cancelled);','(void)pid[i].cancelled;'),
          ('demand only after admission','if(on&&int32_t(now-due)>=0)', 'if(on&&pending&&int32_t(now-due)>=0)'),
          ('saturation wraps','v=UINT64_MAX-v<n?UINT64_MAX:v+n;','v+=n;')]
        header=mutant/'include/signal_v2_pid_diagnostics.h';original=header.read_text()
        for name,a,b in mutations:
            assert a in original;header.write_text(original.replace(a,b,1))
            # Compile failures are not mutation kills.
            exe=tmp/'mutation'
            run([CXX,*FLAGS,*['-I'+str(mutant/p) for p in ('tests/pr65/stubs','tests/host_stubs','include','src')],str(ROOT/'tests/pid_s1.cpp'),*[str(mutant/p) for p in CORE],'-o',str(exe)])
            p=subprocess.run([str(exe)],capture_output=True)
            assert p.returncode!=0,('SURVIVED',name)
            print('PASS mutation detected:',name)
        header.write_text(original)
        base=tmp/'base';base.mkdir()
        archive=subprocess.Popen(['git','archive',BASE],cwd=ROOT,stdout=subprocess.PIPE)
        subprocess.run(['tar','-xf','-','-C',str(base)],stdin=archive.stdout,check=True)
        archive.stdout.close();assert archive.wait()==0
        for source,flags in [('tests/test_signal_v2_owner_live.cpp',['-DM5CAN_SIGNAL_V2_OWNER_CONTINUOUS_LIVE=1','-DM5CAN_SIGNAL_V2_OWNER_AUTO_ARM=1']),('tests/pr65/owner-cadence.cpp',[]),('tests/pr65/owner-cadence.cpp',['-DM5CAN_SIGNAL_V2_10HZ_LAB=1']),('tests/pr65/owner.cpp',[])]:
            f=tmp/'fixture.cpp';f.write_text(fixture(ROOT/source))
            values=[]
            for label,root in [('base',base),('head',ROOT),('extreme',mutant)]:
                if label=='extreme':
                    p=mutant/'src/signal_v2_can_owner.h';body=(ROOT/'src/signal_v2_can_owner.h').read_text()
                    # Extreme telemetry before every tick. Only counters change.
                    assert body.count('    if(!can_)return;\n// P5_BEGIN')==1
                    p.write_text(body.replace('    if(!can_)return;\n// P5_BEGIN','    for(auto &c:pid_diagnostics_.pid){c.demand=c.admitted=c.completed=c.shed=c.denied=c.failed=c.cancelled=UINT64_MAX;}\n    pid_diagnostics_.active_ms=UINT64_MAX;\n    if(!can_)return;\n// P5_BEGIN',1))
                value=compile_run(root,f,tmp/('exe-'+label),flags)
                values.append(value)
            assert values[0]==values[1]==values[2],source
            assert 'TX ' in values[0] and 'RX ' in values[0]
            print('PASS fixed-base + extreme counter TX/RX trace',source,flags,hashlib.sha256(values[0].encode()).hexdigest())
        # Existing production-monitor scenarios include rolling boundaries,
        # full 120ms, FC, all RX charging, RX loss, retry/STOP and lower gate.
        src=(ROOT/'tests/pr65/monitor.cpp').read_text()
        src=src.replace('esp_err_t twai_transmit(const twai_message_t* m,TickType_t) {','esp_err_t twai_transmit(const twai_message_t* m,TickType_t) {\n std::printf("WIRE_TX %u %08x %u",g_test_millis,unsigned(m->identifier),unsigned(m->data_length_code));for(unsigned i=0;i<m->data_length_code;++i){std::printf(" %02x",unsigned(m->data[i]));}std::puts("");')
        src=src.replace('*m=rx.front().frame;', '*m=rx.front().frame;std::printf("WIRE_RX %u %08x %u",g_test_millis,unsigned(m->identifier),unsigned(m->data_length_code));for(unsigned j=0;j<m->data_length_code;++j){std::printf(" %02x",unsigned(m->data[j]));}std::puts("");')
        f=tmp/'monitor.cpp';f.write_text(src)
        values=[compile_run(root,f,tmp/('monitor'+label),['-DM5CAN_ECU_SELECTION_EXPERIMENTAL=1','-DM5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL=1'],True) for label,root in [('base',base),('head',ROOT)]]
        assert values[0]==values[1] and 'WIRE_TX ' in values[0]
        print('PASS fixed-base production monitor wire trace',hashlib.sha256(values[0].encode()).hexdigest())
if __name__=='__main__':main()
