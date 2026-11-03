#!/usr/bin/env python3
"""Explain every owner_live LOAD-byte difference of PID S1 (PM Claude 2026-11-03).
No devices. Inputs are four ELFs of env m5dial_signal_v2_can_29bit_5hz_owner_live,
built from the same checkout path and toolchain:
  --base / --head            normal builds of base 3e43dfe and the S1 head;
  --base-relocs / --head-relocs  the same sources linked with -Wl,--emit-relocs.
1. --emit-relocs only keeps relocation sections: every LOAD section of a
   *-relocs ELF must be byte-identical to its normal ELF.
2. Every LOAD section is split into symbol segments (symbol start to next
   symbol start, so literal pools and padding are covered). Bytes patched by a
   linker relocation are masked and that relocation is compared symbolically
   (target segment + offset). Unpatched bytes must be identical.
3. A remaining difference is allowed only in a segment named in --allow (the
   owner code that contains the S1 hooks / counter storage / read-out), or the
   single 40-byte build SHA in .flash.rodata. Anything else fails.
"""
import argparse, bisect, collections, re, subprocess, pathlib

def loads(path,prefix):
    blob=path.read_bytes();lines=subprocess.check_output([prefix+'objdump','-h',str(path)],text=True).splitlines()
    out={}
    for i,line in enumerate(lines[:-1]):
        m=re.match(r'\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)',line)
        if m and 'LOAD' in lines[i+1]:
            name,size,vma,lma,off=m.groups()
            out[name]=(int(vma,16),int(lma,16),blob[int(off,16):int(off,16)+int(size,16)])
    return out

def symbols(path,prefix):
    syms=[]
    for line in subprocess.check_output([prefix+'nm','-n','-C','--defined-only',str(path)],text=True).splitlines():
        m=re.match(r'([0-9a-f]+) (\w) (.*)$',line)
        if m and m.group(2) not in 'aUwN':syms.append((int(m.group(1),16),m.group(3)))
    return syms

def relocs(path,prefix):
    out=[]
    text=subprocess.check_output([prefix+'readelf','-W','-r',str(path)],text=True)
    for line in text.splitlines():
        m=re.match(r'\s*([0-9a-f]{8})\s+[0-9a-f]+\s+(R_XTENSA_\w+)\s+([0-9a-f]{8})\s+(.*?)\s*([+-])\s*([0-9a-f]+)\s*$',line)
        if not m:
            m2=re.match(r'\s*([0-9a-f]{8})\s+[0-9a-f]+\s+(R_XTENSA_\w+)\s*([0-9a-f]+)?\s*$',line)
            if m2:out.append((int(m2.group(1),16),m2.group(2),int(m2.group(3) or '0',16)))
            continue
        off,typ,val,_,sign,add=m.groups()
        out.append((int(off,16),typ,int(val,16)+(int(add,16) if sign=='+' else -int(add,16))))
    return out

def width(typ,blob,i):
    if typ=='R_XTENSA_32':return 4
    if typ.startswith('R_XTENSA_SLOT') and typ.endswith('_OP'):
        return 2 if (blob[i]&0x0f)>=8 else 3 # Xtensa op0>=8: 16-bit density op.
    if typ in ('R_XTENSA_ASM_EXPAND','R_XTENSA_NONE','R_XTENSA_ASM_SIMPLIFY'):return 0
    raise AssertionError('unknown relocation type in LOAD: '+typ)

class Image:
    def __init__(s,elf,relocs_elf,prefix):
        s.load=loads(elf,prefix);rl=loads(relocs_elf,prefix)
        assert s.load==rl,('--emit-relocs changed LOAD bytes',elf)
        s.syms=symbols(relocs_elf,prefix);s.addrs=[a for a,_ in s.syms]
        s.rel=relocs(relocs_elf,prefix)
    def where(s,addr):
        i=bisect.bisect_right(s.addrs,addr)-1
        if i<0:return ('<none>',addr)
        a=s.addrs[i];names=[n for x,n in s.syms if x==a];return (names[0],addr-a)
    def segments(s,name):
        vma,_,blob=s.load[name];end=vma+len(blob)
        starts=sorted({a for a,_ in s.syms if vma<=a<end}|{vma})
        segs=collections.OrderedDict();seen=collections.Counter()
        for i,a in enumerate(starts):
            b=starts[i+1] if i+1<len(starts) else end
            names=sorted(n for x,n in s.syms if x==a) or ['<section start>']
            key=names[0];seen[key]+=1;key=(key,seen[key])
            segs[key]=(a,b)
        return vma,blob,segs
    def view(s,name):
        vma,blob,segs=s.segments(name)
        bysec=[r for r in s.rel if vma<=r[0]<vma+len(blob)]
        out=collections.OrderedDict();starts=[a for a,_ in segs.values()];keys=list(segs)
        rel_by=collections.defaultdict(list)
        for off,typ,target in bysec:
            k=keys[bisect.bisect_right(starts,off)-1];rel_by[k].append((off,typ,target))
        for k,(a,b) in segs.items():
            data=bytearray(blob[a-vma:b-vma]);rels=[]
            for off,typ,target in sorted(rel_by[k]):
                w=width(typ,blob,off-vma)
                for j in range(w):
                    if off-a+j<len(data):data[off-a+j]=0
                if w:rels.append((off-a,typ,s.where(target)))
            out[k]=(bytes(data),tuple(rels),b-a)
        return out

def main():
    p=argparse.ArgumentParser()
    for n in ('base','head','base-relocs','head-relocs'):p.add_argument('--'+n,type=pathlib.Path,required=True)
    p.add_argument('--tool-prefix',required=True)
    p.add_argument('--allow',action='append',default=[],help='regex of segment names allowed to differ')
    a=p.parse_args()
    base=Image(a.base,a.base_relocs,a.tool_prefix);head=Image(a.head,a.head_relocs,a.tool_prefix)
    print('PASS --emit-relocs LOAD bytes identical to the normal builds (base and head)')
    assert base.load.keys()==head.load.keys()
    allow=[re.compile(x) for x in a.allow];unexplained=[];explained=[]
    sha=re.compile(rb'(?<![0-9a-f])[0-9a-f]{40}(?=\x00#BOOT,git=)')
    for name in base.load:
        bv,bl,bb=base.load[name];hv,hl,hb=head.load[name]
        print('%-16s base=%7d head=%7d delta=%+5d VMA %08x->%08x LMA %08x->%08x identical=%s'%(name,len(bb),len(hb),len(hb)-len(bb),bv,hv,bl,hl,bb==hb))
        if bb==hb:continue
        bs,hs=base.view(name),head.view(name)
        for key in list(bs)+[k for k in hs if k not in bs]:
            x,y=bs.get(key),hs.get(key)
            if x and y and x[0]==y[0] and x[1]==y[1]:continue
            label='%s#%d'%key
            if name=='.flash.rodata' and x and y and len(x[0])==len(y[0]):
                d=[i for i in range(len(x[0])) if x[0][i]!=y[0][i]]
                mb,mh=sha.search(x[0]),sha.search(y[0])
                if x[1]==y[1] and d and mb and mh and mb.start()==mh.start() and d[0]>=mb.start() and d[-1]<mb.end():
                    explained.append((name,label,'build SHA string %s -> %s'%(mb.group().decode(),mh.group().decode())));continue
            why=('only in head' if not x else 'only in base' if not y else
                 'size %d->%d'%(x[2],y[2]) if x[2]!=y[2] else
                 'unrelocated bytes' if x[0]!=y[0] else 'relocation targets')
            if any(r.search(key[0]) for r in allow):explained.append((name,label,why))
            else:unexplained.append((name,label,why))
    for e in explained:print('EXPLAINED',*e)
    for u in unexplained:print('UNEXPLAINED',*u)
    assert not unexplained,'unexplained LOAD differences'
    for label,path in (('base',a.base),('head',a.head)):
        names=subprocess.check_output([a.tool_prefix+'nm','-C','--defined-only',str(path)],text=True)
        count=sum(bool(re.search(r'Discovery11|discovery11_|performDiscovery11|armDiscovery11|tickDiscovery11',l)) for l in names.splitlines())
        assert count==0;print('PASS',label,'owner_live discovery symbols=0')
    print('PASS every LOAD difference is in an allowed S1 segment or the build SHA; all other bytes identical after symbolic relocation comparison')
if __name__=='__main__':main()
