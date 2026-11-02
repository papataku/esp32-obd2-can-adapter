#!/usr/bin/env python3
"""Compare all flashed LOAD sections, plus ESP image excluding derived hashes.
Build the pinned P3 base with the head's exact build stamp before this check.
"""
import argparse, hashlib, pathlib, re, subprocess, tempfile
p=argparse.ArgumentParser();p.add_argument('base',type=pathlib.Path);p.add_argument('head',type=pathlib.Path);p.add_argument('--tool-prefix',required=True);a=p.parse_args()
def sections(elf):
    lines=subprocess.check_output([a.tool_prefix+'objdump','-h',str(elf)],text=True).splitlines()
    result={}
    for i,line in enumerate(lines[:-1]):
        m=re.match(r'\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)',line)
        if m and 'LOAD' in lines[i+1]:
            name,size,vma,lma,offset=m.groups()
            data=elf.read_bytes()[int(offset,16):int(offset,16)+int(size,16)]
            result[name]=(vma,lma,data)
    return result
old,new=sections(a.base),sections(a.head);assert old.keys()==new.keys()
for name,value in old.items():
    assert value==new[name],name+' changed'
    print('PASS identical LOAD',name,'bytes='+str(len(value[2])),hashlib.sha256(value[2]).hexdigest())
x,y=a.base.with_suffix('.bin').read_bytes(),a.head.with_suffix('.bin').read_bytes()
assert len(x)==len(y),'firmware length changed'
# ESP app descriptor ELF SHA at 0xb0, followed by derived image XOR and SHA.
# All LOAD bytes above, including the build stamp, must already match exactly.
def normalize(b):
    b=bytearray(b);b[0xb0:0xd0]=bytes(32);b[-33:]=bytes(33);return b
assert normalize(x)==normalize(y),'non-metadata firmware bytes changed'
print('PASS firmware bytes identical except derived ELF SHA/image checksums (debug source paths differ)')
print('owner_live firmware.bin SHA256='+hashlib.sha256(y).hexdigest())
