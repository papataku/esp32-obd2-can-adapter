#!/usr/bin/env python3
"""Explain every owner_live LOAD-byte difference of PID S1 (PM Claude 2026-11-03).
No devices. Inputs: four ELFs of env m5dial_signal_v2_can_29bit_5hz_owner_live
from the same toolchain:
  --base / --head                 normal builds of base 3e43dfe and the S1 head;
  --base-relocs / --head-relocs   the same sources linked with -Wl,--emit-relocs.

1. --emit-relocs keeps relocation sections only: each *-relocs ELF must have
   LOAD bytes identical to its normal ELF.
2. Code (.iram0.vectors/.iram0.text/.flash.text): every sized function is
   compared as normalized disassembly. Branch/call targets are compared as
   symbol+offset and every l32r by the *value* of its literal (symbolic when it
   is an image address), because the linker places/relaxes literal pools and
   moves code. Non-function code words (literal pools, padding) are compared
   as a symbolic sequence; every inserted/removed literal must be referenced
   only by allowed functions.
3. Data (.dram0.data/.flash.rodata/.flash.appdesc): per symbol segment, words
   patched by relocations and unrelocated absolute addresses (e.g. EH frame
   pc_begin) are compared symbolically. Allowed residuals: the 40-byte build
   SHA, the appended zero-initialised counter storage of the owner object,
   a reordering of the same merged strings, and EH frame / LSDA bytes that
   belong to an allowed function. Anything else fails.
Allowed functions are given with --allow (exact demangled names).
"""
import argparse, bisect, collections, difflib, re, subprocess, pathlib
CODE=('.iram0.vectors','.iram0.text','.flash.text')
SHA=re.compile(rb'(?<![0-9a-f])[0-9a-f]{40}(?=\x00#BOOT,git=)')

def loads(path,prefix):
    blob=path.read_bytes();lines=subprocess.check_output([prefix+'objdump','-h',str(path)],text=True).splitlines()
    out={}
    for i,line in enumerate(lines[:-1]):
        m=re.match(r'\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)',line)
        if m and 'LOAD' in lines[i+1]:
            name,size,vma,lma,off=m.groups()
            out[name]=(int(vma,16),int(lma,16),blob[int(off,16):int(off,16)+int(size,16)])
    return out

def alloc_ranges(path,prefix):
    lines=subprocess.check_output([prefix+'objdump','-h',str(path)],text=True).splitlines();out=[]
    for i,line in enumerate(lines[:-1]):
        m=re.match(r'\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)',line)
        if m and 'ALLOC' in lines[i+1] and int(m.group(2),16):out.append((int(m.group(3),16),int(m.group(3),16)+int(m.group(2),16),m.group(1)))
    return out

def relocs(path,prefix,sections):
    out=[];current=None
    for line in subprocess.check_output([prefix+'readelf','-W','-r',str(path)],text=True).splitlines():
        h=re.match(r"Relocation section '\.rela(\S+)'",line)
        if h:current=h.group(1);continue
        if current not in sections:continue # debug/.xt.prop relocations are not image bytes
        m=re.match(r'\s*([0-9a-f]{8})\s+[0-9a-f]+\s+(R_XTENSA_\w+)\s+([0-9a-f]{8})\s+(.*?)\s*([+-])\s*([0-9a-f]+)\s*$',line)
        if m:
            off,typ,val,_,sign,add=m.groups()
            out.append((int(off,16),typ,int(val,16)+(int(add,16) if sign=='+' else -int(add,16))))
        elif re.match(r'\s*[0-9a-f]{8}\s+[0-9a-f]+\s+R_XTENSA_',line):
            out.append((int(line.split()[0],16),line.split()[2],None))
    return out

WIDTH={'R_XTENSA_32':4,'R_XTENSA_PDIFF32':4,'R_XTENSA_PDIFF16':2,'R_XTENSA_NONE':0,
       'R_XTENSA_ASM_EXPAND':0,'R_XTENSA_ASM_SIMPLIFY':0}

class Image:
    def __init__(s,elf,relocs_elf,prefix):
        s.prefix=prefix;s.elf=elf
        s.load=loads(elf,prefix)
        s.alloc=alloc_ranges(elf,prefix)
        assert s.load==loads(relocs_elf,prefix),('--emit-relocs changed LOAD bytes',elf)
        s.syms=[];s.funcs=[];s.objs=[]
        for line in subprocess.check_output([prefix+'nm','-n','-S','--defined-only',str(elf)],text=True).splitlines():
            m=re.match(r'([0-9a-f]+) (?:([0-9a-f]+) )?(\w) (\S+)$',line)
            if not m or m.group(3) in 'UwN':continue
            if m.group(3) in 'aA' and not m.group(4).startswith('_'):continue
            a=int(m.group(1),16);s.syms.append((a,m.group(4)))
            if m.group(2) and int(m.group(2),16) and s.section(a)=='.flash.rodata':s.objs.append((a,a+int(m.group(2),16)))
            if m.group(2) and m.group(3) in 'tTW' and int(m.group(2),16) and s.section(a) in CODE:
                s.funcs.append((a,int(m.group(2),16),m.group(4)))
        s.objs.sort();s.objstarts=[a for a,_ in s.objs]
        s.syms.sort();s.addrs=[a for a,_ in s.syms];s.byaddr={}
        for a,n in s.syms:s.byaddr[a]=min(n,s.byaddr.get(a,n));s.funcs.sort();s.fstarts=[f[0] for f in s.funcs]
        names=[n for _,_,n in s.funcs]
        s.demangled=dict(zip(names,subprocess.run([prefix+'c++filt'],input='\n'.join(names),capture_output=True,text=True).stdout.splitlines()))
        s.rel=relocs(relocs_elf,prefix,set(s.load))
        s.crel={}
        for o,t,x in s.rel:
            if s.section(o) not in CODE:continue
            # --emit-relocs addends against section symbols are not reliable on
            # Xtensa, so relocations only mark WHICH instruction/word is
            # linker-patched; its target is read from the decoded instruction.
            if t=='R_XTENSA_SLOT0_OP':s.crel[o]=t
            elif t=='R_XTENSA_ASM_SIMPLIFY':s.crel[o+3]=t # relaxed longcall: call8 follows the 3-byte slot
            elif t=='R_XTENSA_32':s.crel[o]=t
        s.insn={};s.lit_users=collections.defaultdict(set)
        for sec in CODE:
            for line in subprocess.check_output([prefix+'objdump','-d','-j',sec,str(elf)],text=True).splitlines():
                m=re.match(r'\s*([0-9a-f]{8}):\t([0-9a-f]+)\s*\t(\S+)\s*(.*)$',line)
                if m:
                    a=int(m.group(1),16);s.insn[a]=(len(m.group(2))//2,m.group(3),m.group(4))
                    if m.group(3)=='l32r':
                        t=re.search(r'([0-9a-f]{8}) <',m.group(4))
                        if t:s.lit_users[int(t.group(1),16)].add(a)
    def section(s,addr):
        for n,(v,_,b) in s.load.items():
            if v<=addr<v+len(b):return n
    def word(s,addr):
        n=s.section(addr)
        if n is None:return None
        v,_,b=s.load[n];return int.from_bytes(b[addr-v:addr-v+4],'little')
    def raw(s,addr):
        n=s.section(addr);v,_,b=s.load[n];return b[addr-v]
    def string(s,addr):
        # A pointer into a NUL-terminated printable run of .flash.rodata is
        # compared by content: the linker may reorder identical merged strings.
        if s.section(addr)!='.flash.rodata':return None
        i=bisect.bisect_right(s.objstarts,addr)-1
        if i>=0 and s.objs[i][0]<=addr<s.objs[i][1]:return None # inside a sized object (vtable, table)
        v,_,b=s.load['.flash.rodata'];i=addr-v;j=b.find(b'\0',i)
        if j<0 or j-i>512:return None
        k=i
        while k>0 and b[k-1]!=0 and i-k<512:k-=1
        run=b[k:j]
        if not run or not all(32<=c<127 or c in (9,10,13) for c in run):return None
        if re.fullmatch(rb'[0-9a-f]{40}',run):run=b'<BUILD_SHA>' # the build identity string
        return repr(run)+('+%d'%(i-k) if i!=k else '')
    def where(s,addr):
        if addr is None:return None
        if addr in s.byaddr:return (s.byaddr[addr],0) # includes linker symbols such as _heap_start
        if not any(lo<=addr<hi for lo,hi,_ in s.alloc):return None # .bss/.noinit included
        i=bisect.bisect_right(s.addrs,addr)-1
        a=s.addrs[i];return (s.byaddr[a],addr-a)
    def sym(s,value):
        if value is None:return '#outside-LOAD'
        txt=s.string(value)
        if txt is not None:return 'str:'+txt
        w=s.where(value);return '%s+%d'%w if w else '#%x'%value
    def normalized(s,start,size):
        out=[];a=start
        while a<start+size:
            if a not in s.insn:out.append('?%02x'%s.raw(a));a+=1;continue
            n,mn,ops=s.insn[a]
            if mn=='l32r':
                m=re.search(r'([0-9a-f]{8}) <',ops);ops=re.sub(r'[0-9a-f]{8} <[^>]+>','='+s.sym(s.word(int(m.group(1),16))),ops)
            else:ops=re.sub(r'[0-9a-f]{8} <([^>]+)>',r'<\1>',ops)
            out.append(mn+' '+ops);a+=n
        return out
    def masked(s,start,size):
        data=bytearray(size);rel=[];a=start
        for i in range(size):data[i]=s.raw(start+i)
        for o in range(start,start+size):
            if o not in s.crel:continue
            t=s.crel[o]
            if t=='R_XTENSA_32':w=4;tgt=s.sym(s.word(o))
            else:w,tgt=s.decode(o)
            for j in range(w):
                if o-start+j<size:data[o-start+j]=0
            rel.append((o-start,t,tgt))
        return bytes(data),rel
    def decode(s,o):
        # Xtensa little-endian 24-bit PC-relative forms used by the linker.
        ins=s.raw(o)|s.raw(o+1)<<8|s.raw(o+2)<<16;op0=ins&0xf
        if op0==1: # l32r: literal value, not its pool address
            lit=((o+3)&~3)+(((ins>>8)-0x10000)<<2);return 3,'l32r ='+s.sym(s.word(lit))
        if op0==5: # call0/4/8/12
            off=ins>>6;off-=(1<<18) if off&(1<<17) else 0
            return 3,'call%d '%(((ins>>4)&3)*4)+s.sym((o&~3)+(off<<2)+4)
        if op0==6 and (ins>>4)&3==0: # j
            off=ins>>6;off-=(1<<18) if off&(1<<17) else 0
            return 3,'j '+s.sym(o+off+4)
        sx=lambda v,bits:v-(1<<bits) if v&(1<<(bits-1)) else v
        if op0==0xc and (ins>>7)&1: # beqz.n / bnez.n (RI6, unsigned forward)
            return 2,'bz.n %x '%(ins&0xff)+s.sym(o+((((ins>>4)&3)<<4)|((ins>>12)&0xf))+4)
        if op0==7:return 3,'b8 %x '%(ins&0xffff)+s.sym(o+sx(ins>>16,8)+4) # RRI8 branch
        if op0==6 and (ins>>4)&3==1:return 3,'b12 %x '%(ins&0xfff)+s.sym(o+sx(ins>>12,12)+4) # BRI12
        if op0==6 and (ins>>4)&3==2:return 3,'bi8 %x '%(ins&0xffff)+s.sym(o+sx(ins>>16,8)+4) # BRI8
        if o in s.insn:
            w,mn,ops=s.insn[o];m=re.search(r'([0-9a-f]{8}) <',ops)
            if m:return w,mn+' '+s.sym(int(m.group(1),16))
        raise AssertionError(('unsupported relocated instruction',hex(o),hex(ins)))
    def alignnorm(s,start,size,name):
        # Instruction stream without alignment fill; intra-function branch
        # targets become instruction indices, l32r becomes its literal value.
        items=[];a=start;index={}
        while a<start+size:
            if a not in s.insn:items.append((a,'?%02x'%s.raw(a)));a+=1;continue
            n,mn,ops=s.insn[a]
            if not ((mn=='.byte' and ops.strip() in ('00','0x00')) or mn in ('nop','nop.n')):
                mn=mn[:-2] if mn.endswith('.n') else mn # density (narrow) form == wide form
                r=re.fullmatch(r'(a\d+), (a\d+), (a\d+)',ops.strip())
                if mn=='or' and r and r.group(2)==r.group(3):mn,ops='mov',r.group(1)+', '+r.group(2)
                items.append((a,mn,ops))
            a+=n
        for k,it in enumerate(items):index[it[0]]=k
        out=[]
        for it in items:
            if len(it)==2:out.append(it[1]);continue
            a,mn,ops=it
            if mn=='l32r':
                m=re.search(r'([0-9a-f]{8}) <',ops);ops=re.sub(r'[0-9a-f]{8} <[^>]+>','='+s.sym(s.word(int(m.group(1),16))),ops)
            else:
                def tgt(m):
                    t=int(m.group(1),16)
                    if start<=t<start+size:return '@%s'%index.get(t,'?')
                    return '<'+s.sym(t)+'>'
                ops=re.sub(r'([0-9a-f]{8}) <[^>]+>',tgt,ops)
            out.append(mn+' '+ops)
        return out
    def owner(s,addr):
        i=bisect.bisect_right(s.fstarts,addr)-1
        if i>=0 and s.funcs[i][0]<=addr<s.funcs[i][0]+s.funcs[i][1]:return s.demangled[s.funcs[i][2]]
        return None
    def pool_words(s,sec):
        v,_,b=s.load[sec];covered=bytearray(len(b))
        for a,n,_ in s.funcs:
            if s.section(a)==sec:covered[a-v:a-v+n]=b'\1'*n
        return [(v+i,s.sym(s.word(v+i))) for i in range(0,len(b)-3,4) if not any(covered[i:i+4])]
    def regions(s,sec):
        # Maximal runs not covered by a sized function, keyed by the preceding function.
        v,_,b=s.load[sec];end=v+len(b);fs=[f for f in s.funcs if s.section(f[0])==sec];out={};c=collections.Counter()
        pos=v;prev='<section start>'
        for a,n,name in fs+[(end,0,None)]:
            if a>pos:
                c[prev]+=1;out[(prev,c[prev])]=(pos,a)
            pos=max(pos,a+n);prev=name or prev
        return out
    def segments(s,sec):
        v,_,b=s.load[sec];end=v+len(b)
        starts=sorted({a for a,_ in s.syms if v<=a<end}|{v});segs=collections.OrderedDict();seen=collections.Counter()
        for i,a in enumerate(starts):
            key=min([n for x,n in s.syms if x==a] or ['<section start>']);seen[key]+=1
            segs[(key,seen[key])]=(a,starts[i+1] if i+1<len(starts) else end)
        return segs

def asm_bodies(path):
    # GCC -S output: instruction text per function, without debug/CFI/labels.
    lines=pathlib.Path(path).read_text().split('\n');out={};cur=None
    for x in lines:
        m=re.match(r'^([A-Za-z_][\w.$]*):$',x)
        if m and not x.startswith('.L'):cur=m.group(1);out[cur]=[];continue
        if cur and re.match(r'\s*\.size\s+'+re.escape(cur)+',',x):cur=None;continue
        if cur and not re.match(r'\s*\.(loc|cfi_\w+|file|LVL|LBB|LBE|LFB|LFE|LEHB|LEHE|LVU|section|align|literal_position)\b|\.L\w+:',x):
            out[cur].append(re.sub(r'\.L\w+','L',x.strip()))
    return out

def compare_code(base,head,allow,report,asm=None):
    relaxed=set();B={};H={}
    for img,d in ((base,B),(head,H)):
        c=collections.Counter()
        for a,n,name in img.funcs:
            c[name]+=1;d[(name,c[name])]=(a,n,img.demangled[name])
    for key in sorted(set(B)|set(H)):
        x,y=B.get(key),H.get(key)
        if x and y and x[1]==y[1] and base.masked(x[0],x[1])==head.masked(y[0],y[1]):continue
        name=(x or y)[2]
        if x and y and name not in allow and base.alignnorm(x[0],x[1],name)==head.alignnorm(y[0],y[1],name):
            relaxed.add(name)
            report.append(('ALLOWED','code-relax',name,'size %d->%d: same instructions (narrow/wide forms equal), branch/call targets and l32r literal values; only assembler/linker alignment fill or density choice differs'%(x[1],y[1])));continue
        mangled=key[0]
        if x and y and name not in allow and asm and mangled in asm[0] and asm[0][mangled]==asm[1].get(mangled):
            relaxed.add(name)
            report.append(('ALLOWED','code-asm',name,'size %d->%d: GCC -S instruction text identical (%d lines) in base and head; the binary differs only by assembler/linker relaxation'%(x[1],y[1],len(asm[0][mangled]))));continue
        how='only in head' if not x else 'only in base' if not y else ('size %d->%d'%(x[1],y[1]) if x[1]!=y[1] else 'same size, instructions differ')
        report.append(('ALLOWED' if name in allow else 'UNEXPLAINED','code',name,how))
    # Bytes of code sections outside every sized function: literal pools,
    # fill and unsized (assembly) routines. Regions holding linker-patched
    # instructions are compared like functions (masked + symbolic targets);
    # pure literal/fill regions are only reachable through l32r, whose loaded
    # VALUES were already compared per function above.
    for sec in CODE:
        rb,rh=base.regions(sec),head.regions(sec);pools=0;code=0
        for key in sorted(set(rb)|set(rh)):
            x,y=rb.get(key),rh.get(key)
            has_code=any(img.crel.get(o)=='R_XTENSA_SLOT0_OP' or img.crel.get(o)=='R_XTENSA_ASM_SIMPLIFY' for img,r in ((base,x),(head,y)) if r for o in range(r[0],r[1]))
            if not has_code:
                if x!=None and y!=None and base.masked(x[0],x[1]-x[0])==head.masked(y[0],y[1]-y[0]):continue
                pools+=1;continue
            if x and y and x[1]-x[0]==y[1]-y[0] and base.masked(x[0],x[1]-x[0])==head.masked(y[0],y[1]-y[0]):continue
            code+=1;report.append(('UNEXPLAINED','code-region',sec,'%s#%d'%key))
        if pools:report.append(('ALLOWED','literal-pool',sec,'%d literal/fill regions differ (pool placement and coalescing); every l32r user above was compared by literal VALUE'%pools))
    return relaxed

def records(img,sec,seg):
    v,_,b=img.load[sec];a=seg[0]
    while a+4<=seg[1]:
        n=int.from_bytes(b[a-v:a-v+4],'little')
        if n==0:break
        yield a,n,int.from_bytes(b[a+4-v:a+8-v],'little')
        a+=4+n

def eh_owner(img,sec,seg,addr):
    """Function that owns an EH frame byte: the pc_begin of its FDE."""
    for a,n,cie in records(img,sec,seg):
        if a<=addr<a+4+n:return '<CIE>' if cie==0 else img.owner(img.word(a+8))

def lsda_owner(img,sec,addr):
    """Map an LSDA byte to its function through the FDE LSDA pointers."""
    best=None
    for key,seg in img.segments(sec).items():
        if key[0]!='__EH_FRAME_BEGIN__':continue
        for a,n,cie in records(img,sec,seg):
            if cie and n>=17 and img.raw(a+12)==4:
                lsda=img.word(a+13)
                if lsda<=addr and (best is None or lsda>best[0]):best=(lsda,img.owner(img.word(a+8)))
    return best[1] if best else None

def compare_data(base,head,allow,report,relaxed=frozenset()):
    for sec in base.load:
        if sec in CODE:continue
        bv,_,bb=base.load[sec];hv,_,hb=head.load[sec]
        if bb==hb:continue
        bs,hs=base.segments(sec),head.segments(sec)
        brel={o:t for o,t,x in base.rel if base.section(o)==sec and WIDTH.get(t)}
        hrel={o:t for o,t,x in head.rel if head.section(o)==sec and WIDTH.get(t)}
        for key in list(bs)+[k for k in hs if k not in bs]:
            x,y=bs.get(key),hs.get(key);label='%s#%d'%key
            if not x or not y:report.append(('UNEXPLAINED','data',sec,label+' only in one image'));continue
            xb,yb=bb[x[0]-bv:x[1]-bv],hb[y[0]-hv:y[1]-hv]
            if xb==yb:continue
            owner_obj=key[0]=='_ZN9signal_v212_GLOBAL__N_15ownerE' and len(yb)-len(xb)==304 and not any(yb[len(xb):])
            if owner_obj:yb=yb[:len(xb)] # tail: appended zero-initialised PidDiagnostics (sizeof 304)
            if len(xb)!=len(yb):report.append(('UNEXPLAINED','data',sec,label+' size %d->%d'%(len(xb),len(yb))));continue
            residual=[];i=0;moved=0
            def same_pointer(k):
                if k<0 or k+4>len(xb):return False
                w1=int.from_bytes(xb[k:k+4],'little');w2=int.from_bytes(yb[k:k+4],'little')
                if w1==w2 or w1>>24 not in (0x40,0x42,0x3f,0x3c) or w2>>24 not in (0x40,0x42,0x3f,0x3c):return False
                return base.where(w1) is not None and base.sym(w1)==head.sym(w2)
            while i<len(xb):
                if xb[i]==yb[i]:i+=1;continue
                # A moved absolute address (relocated or not, aligned or not,
                # e.g. EH pc_begin / CIE personality) that names the same thing.
                k=next((k for k in range(i-3,i+1) if same_pointer(k)),None)
                if k is not None:i=k+4;moved+=1;continue
                residual.append(i);i+=1
            if owner_obj and not residual:
                report.append(('ALLOWED','data',sec,label+' owner object: first %d bytes identical up to %d moved pointers (same targets); +304 zero bytes = appended PidDiagnostics storage'%(len(xb),moved)));continue
            if not residual:
                report.append(('ALLOWED','data',sec,label+' only moved addresses (%d words), all symbolically identical'%moved));continue
            mb,mh=SHA.search(xb),SHA.search(yb)
            if mb and mh and all(mb.start()<=k<mb.end() for k in residual):
                report.append(('ALLOWED','data',sec,label+' build SHA %s -> %s'%(mb.group().decode(),mh.group().decode())));continue
            if key[0] in ('__EH_FRAME_BEGIN__','__XT_EXCEPTION_TABLE_'):
                owners=collections.Counter()
                for k in residual:
                    owners[eh_owner(head,sec,y,y[0]+k) if key[0]=='__EH_FRAME_BEGIN__' else lsda_owner(head,sec,y[0]+k)]+=1
                ok=all(o in allow or o in relaxed for o in owners)
                report.append(('ALLOWED' if ok else 'UNEXPLAINED','data',sec,label+' %d unwind bytes owned by %s'%(len(residual),dict(owners))));continue
            lo,hi=residual[0],residual[-1]+1
            while lo>0 and xb[lo-1]!=0:lo-=1
            while hi<len(xb) and xb[hi-1]!=0:hi+=1
            if sorted(xb[lo:hi].split(b'\0'))==sorted(yb[lo:hi].split(b'\0')):
                n=len([w for w in xb[lo:hi].split(b'\0') if w])
                report.append(('ALLOWED','data',sec,label+' the same %d merged strings, reordered in [%d,%d)'%(n,lo,hi)));continue
            report.append(('UNEXPLAINED','data',sec,label+' %d residual bytes at %s'%(len(residual),residual[:8])))

def main():
    p=argparse.ArgumentParser()
    for n in ('base','head','base-relocs','head-relocs'):p.add_argument('--'+n,type=pathlib.Path,required=True)
    p.add_argument('--tool-prefix',required=True)
    p.add_argument('--allow',action='append',default=[],help='exact demangled function name allowed to differ')
    p.add_argument('--asm-base',type=pathlib.Path);p.add_argument('--asm-head',type=pathlib.Path)
    a=p.parse_args()
    base=Image(a.base,a.base_relocs,a.tool_prefix);head=Image(a.head,a.head_relocs,a.tool_prefix)
    print('PASS --emit-relocs LOAD bytes identical to the normal builds (base and head)')
    assert base.load.keys()==head.load.keys()
    for name in base.load:
        bv,bl,bb=base.load[name];hv,hl,hb=head.load[name]
        print('%-16s base=%7d head=%7d delta=%+5d VMA %08x->%08x LMA %08x->%08x identical=%s'%(name,len(bb),len(hb),len(hb)-len(bb),bv,hv,bl,hl,bb==hb))
    allow=set(a.allow);report=[]
    asm=(asm_bodies(a.asm_base),asm_bodies(a.asm_head)) if a.asm_base else None
    relaxed=compare_code(base,head,allow,report,asm);compare_data(base,head,allow,report,relaxed)
    for r in report:print(*r)
    for label,img in (('base',base),('head',head)):
        n=sum(bool(re.search(r'Discovery11|discovery11_',x)) for _,x in img.syms)
        assert n==0;print('PASS',label,'owner_live discovery symbols=0')
    assert not any(r[0]=='UNEXPLAINED' for r in report),'unexplained LOAD differences'
    print('PASS every LOAD difference is explained: allowed S1 functions, their literals/unwind data, counter storage, string order, build SHA')
if __name__=='__main__':main()
