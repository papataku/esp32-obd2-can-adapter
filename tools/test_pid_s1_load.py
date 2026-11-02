#!/usr/bin/env python3
"""Explain S1 LOAD changes using a diagnostic-erased fixed-base control build.
The control uses the exact base owner and config. Stamp strings must be clean
40-byte git SHAs; every other control LOAD byte must match the pinned base.
"""
import argparse, hashlib, pathlib, re, subprocess

def sections(path,prefix):
    blob=path.read_bytes()
    lines=subprocess.check_output([prefix+'objdump','-h',str(path)],text=True).splitlines()
    result={}
    for i,line in enumerate(lines[:-1]):
        m=re.match(r'\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)',line)
        if m and 'LOAD' in lines[i+1]:
            name,size,vma,lma,offset=m.groups()
            result[name]=(vma,lma,blob[int(offset,16):int(offset,16)+int(size,16)])
    return result

def main():
    p=argparse.ArgumentParser()
    for name in ('base','head','erased'):p.add_argument('--'+name,type=pathlib.Path,required=True)
    p.add_argument('--tool-prefix',required=True);a=p.parse_args()
    trees=[sections(path,a.tool_prefix) for path in (a.base,a.head,a.erased)]
    base,head,erased=trees
    assert base.keys()==head.keys()==erased.keys()
    stamp=re.compile(rb'(?<![0-9a-f])[0-9a-f]{40}\x00#BOOT,git=')
    for name,(v,l,data) in base.items():
        cv,cl,control=erased[name]
        if name=='.flash.rodata':
            matches=[stamp.findall(b) for b in (data,control)]
            assert len(matches[0])==len(matches[1])==1
            control=control.replace(matches[1][0],matches[0][0],1)
        assert (cv,cl,control)==(v,l,data),('erased control differs',name)
        hv,hl,new=head[name]
        print(name,'base='+str(len(data)),'head='+str(len(new)),
              'delta='+str(len(new)-len(data)),'VMA='+v+'->'+hv,'LMA='+l+'->'+hl,
              'identical='+str(data==new),'head_sha256='+hashlib.sha256(new).hexdigest())
    print('PASS erased control: ALL LOAD bytes/addresses identical to fixed base except one 40-byte build SHA')
    for label,path in [('base',a.base),('head',a.head)]:
        symbols=subprocess.check_output([a.tool_prefix+'nm','-S','-C','--defined-only',str(path)],text=True)
        count=sum(bool(re.search(r'Discovery11|discovery11_|performDiscovery11|armDiscovery11|tickDiscovery11',line)) for line in symbols.splitlines())
        assert count==0
        print('PASS',label,'owner_live discovery symbols=0')
        for line in symbols.splitlines():
            if re.search(r'PidDiagnostics::|CanOwner::(?:tick|stop|result)|g_can_owner$',line):print(label,line)
    print('S1 additions: appended counter storage, demand/admit/finish/cancel/shed/deny updates and const diagnostic read-out only; remaining LOAD differences are linked addresses/alignment/build identity. Exact source restoration and TX/RX equivalence are checked by test_pid_s1_scope.py and test_pid_s1.py.')
if __name__=='__main__':main()
