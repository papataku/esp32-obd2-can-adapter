#!/usr/bin/env python3
"""Compare production host + serial-HIL transcripts to an immutable Git base.
No serial ports/devices. --mutation-proof changes only a temporary source copy.
"""
import argparse
import difflib
import hashlib
import pathlib
import shutil
import subprocess
import tempfile
import sys

ROOT=pathlib.Path(__file__).resolve().parents[1]
BASE='de635e9f628d46e26050019c15cf337482789276'
FLAGS=['-DM5CAN_SIGNAL_V2_CAN_EXPERIMENTAL=1','-DM5CAN_ECU_SELECTION_EXPERIMENTAL=1',
       '-DM5CAN_SIGNAL_V2_RP8_PROFILE=1','-DM5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL=1']
SOURCES=['src/diagnostic_policy.cpp','src/diagnostic_transaction.cpp','src/isotp_reassembler.cpp']

def run(cmd,**kw):
    p=subprocess.run(cmd,text=True,capture_output=True,**kw)
    if p.returncode:raise RuntimeError('command failed: '+' '.join(map(str,cmd))+'\n'+p.stdout+p.stderr)
    return p.stdout

def transcript(root,work,mode,hz):
    defs=FLAGS+(['-DM5CAN_SIGNAL_V2_10HZ_LAB=1'] if hz==10 else [])
    common=['c++','-std=c++17','-Wall','-Wextra','-Werror','-pthread',*defs,
            '-I'+str(root/'tests/hil/stubs'),'-I'+str(root/'tests/host_stubs'),
            '-I'+str(root/'include'),'-I'+str(root/'src')]
    exe=work/(mode+str(hz))
    if mode=='host':
        # Existing asserted owner-live fixture, extended with observable requests
        # and real owner budget counters; no assertion is removed.
        text=(ROOT/'tests/test_signal_v2_owner_live.cpp').read_text()
        text=text.replace('requests.push_back(q); return true;',
            'std::cout<<"REQUEST "<<q.can_id<<","<<q.timeout_ms<<","<<q.authorization_epoch<<":";'
            'for(unsigned i=0;i<q.length;++i)std::cout<<unsigned(q.data[i])<<",";'
            'std::cout<<"\\n";requests.push_back(q); return true;')
        text=text.replace('int main() {','namespace signal_v2 { struct CanOwnerTestAccess {'
            'static void dump(const CanOwner& o){std::cout<<"BUDGET "<<o.budget_.used100<<","<<o.budget_.used1000<<"\\n";}};}\nint main() {')
        text=text.replace('completeGroup();response.completed_at_us=420000;',
            'signal_v2::CanOwnerTestAccess::dump(owner);completeGroup();response.completed_at_us=420000;')
        fixture=work/'host.cpp';fixture.write_text(text)
        run([*common,'-DM5CAN_SIGNAL_V2_OWNER_CONTINUOUS_LIVE=1','-DM5CAN_SIGNAL_V2_OWNER_AUTO_ARM=1',
             str(fixture),*[str(root/s) for s in SOURCES],'-o',str(exe)])
    else:
        common+=['-DM5CAN_SIGNAL_V2_SERIAL_HIL=1','-DM5CAN_HIL_HOST_TEST=1']
        obj=work/('shim'+str(hz)+'.o')
        run([*common,'-Dtransmit=shimTransmit','-Dreceive=shimReceive','-c',str(root/'src/twai_hil.cpp'),'-o',str(obj)])
        run([*common,str(ROOT/'tests/p2/serial_equivalence.cpp'),str(root/'src/can_monitor.cpp'),
             *[str(root/s) for s in SOURCES],str(obj),'-o',str(exe)])
    return run([str(exe)])

def compare(base,current,out):
    all_equal=True
    for mode in ('host','serial'):
        for hz in (5,10):
            a=out/('base-'+mode+str(hz));b=out/('head-'+mode+str(hz));a.mkdir();b.mkdir()
            old=transcript(base,a,mode,hz);new=transcript(current,b,mode,hz)
            (out/(mode+str(hz)+'-base.txt')).write_text(old)
            (out/(mode+str(hz)+'-head.txt')).write_text(new)
            equal=old==new;all_equal &= equal
            print(('PASS' if equal else 'FAIL'),mode,hz,'Hz',hashlib.sha256(new.encode()).hexdigest())
            if not equal:print(''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile=BASE,tofile='head')))
    return all_equal

def main():
    p=argparse.ArgumentParser();p.add_argument('--mutation-proof',action='store_true');p.add_argument('--output',type=pathlib.Path);args=p.parse_args()
    with tempfile.TemporaryDirectory() as d:
        work=pathlib.Path(d);base=work/'base';base.mkdir()
        if subprocess.run(['git','cat-file','-e',BASE+'^{commit}'],cwd=ROOT,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode:
            run(['git','fetch','--no-tags','origin',BASE],cwd=ROOT)
        archive=subprocess.Popen(['git','archive',BASE],cwd=ROOT,stdout=subprocess.PIPE)
        subprocess.run(['tar','-xf','-','-C',str(base)],stdin=archive.stdout,check=True);archive.stdout.close()
        assert archive.wait()==0
        current=ROOT
        if args.mutation_proof:
            current=work/'mutant';shutil.copytree(ROOT,current,ignore=shutil.ignore_patterns('.git','.pio','__pycache__'))
            header=current/'include/signal_v2_vehicle_profile.h'
            header.write_text(header.read_text().replace('200,5000,25000','200,4999,25000'))
            print('TEMPORARY MUTATION bits_100ms 5000 -> 4999 (source tree untouched)')
        out=args.output or work/'results';out.mkdir(parents=True,exist_ok=True)
        equal=compare(base,current,out)
        # Tight boundary must be observed even if the normal traffic is below
        # both limits. Compile the same budget boundary driver against each.
        fixture=work/'budget.cpp';fixture.write_text('#include "signal_v2_can_budget.h"\n#include <iostream>\nint main(){signal_v2::CanBudget b;std::cout<<b.reserve(1,25,120)<<","<<b.used100<<"\\n";}\n')
        values=[]
        for index,root in enumerate((base,current)):
            exe=work/('budget'+str(index));run(['c++','-std=c++11','-I'+str(root/'include'),'-I'+str(root/'src'),str(fixture),'-o',str(exe)])
            values.append(run([str(exe)]))
        print('budget boundary base='+values[0].strip()+' head='+values[1].strip())
        equal &= values[0]==values[1]
        if not equal:print('FAIL fixed-base equivalence');return 1
        print('PASS fixed-base equivalence',BASE);return 0
if __name__=='__main__':sys.exit(main())
