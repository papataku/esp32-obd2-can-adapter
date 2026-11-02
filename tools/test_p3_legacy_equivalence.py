#!/usr/bin/env python3
"""Compile fixed base code, compare old-client PDUs/states; mutate temp copy only."""
import argparse, difflib, hashlib, pathlib, shutil, subprocess, tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
BASE='18188eed1b45534a7aadc6a2c390f2d594d92921'
def run(cmd,**kw):
    p=subprocess.run(cmd,text=True,capture_output=True,**kw)
    if p.returncode:raise RuntimeError(p.stdout+p.stderr)
    return p.stdout

def main():
    p=argparse.ArgumentParser();p.add_argument('--mutation-proof',action='store_true');p.add_argument('--output',type=pathlib.Path);a=p.parse_args()
    with tempfile.TemporaryDirectory() as d:
        temp=pathlib.Path(d);base=temp/'base';base.mkdir()
        if subprocess.run(['git','cat-file','-e',BASE+'^{commit}'],cwd=ROOT,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode:
            run(['git','fetch','--no-tags','origin',BASE],cwd=ROOT)  # shallow CI checkout
        archive=subprocess.Popen(['git','archive',BASE],cwd=ROOT,stdout=subprocess.PIPE)
        subprocess.run(['tar','-xf','-','-C',str(base)],stdin=archive.stdout,check=True);archive.stdout.close();assert archive.wait()==0
        head=ROOT
        if a.mutation_proof:
            head=temp/'mutant';shutil.copytree(ROOT,head,ignore=shutil.ignore_patterns('.git','.pio','__pycache__'))
            path=head/'src/signal_v2_session.cpp';s=path.read_text();assert 'w16(reply, 512);' in s
            path.write_text(s.replace('w16(reply, 512);','w16(reply, 511);',1))
            print('TEMPORARY MUTATION HELLO max PDU 512 -> 511; working source untouched')
        out=a.output or temp/'results';out.mkdir(exist_ok=True,parents=True);equal=True
        for mode in ('demo','can'):
            values=[]
            for label,root in [('base',base),('head',head)]:
                exe=temp/(mode+label)
                flags=['-DM5CAN_SIGNAL_V2_CAN_EXPERIMENTAL=1','-DM5CAN_SIGNAL_V2_RP8_PROFILE=1'] if mode=='can' else []
                run(['c++','-std=c++17','-Wall','-Wextra','-Werror',*flags,'-I'+str(root/'include'),str(ROOT/'tests/p3/legacy.cpp'),str(root/'src/signal_v2_codec.cpp'),str(root/'src/signal_v2_session.cpp'),'-o',str(exe)])
                value=run([str(exe)]);values.append(value);(out/(mode+'-'+label+'.txt')).write_text(value)
            ok=values[0]==values[1];equal &= ok
            print('PASS' if ok else 'FAIL',mode,'PDUs+active/lease/revision/demand',hashlib.sha256(values[1].encode()).hexdigest())
            if not ok:print(''.join(difflib.unified_diff(values[0].splitlines(True),values[1].splitlines(True),fromfile=BASE,tofile='head')))
        print('PASS' if equal else 'FAIL','fixed-base legacy equivalence',BASE)
        return 0 if equal else 1
if __name__=='__main__':raise SystemExit(main())
