#!/usr/bin/env python3
"""Per-function normalized disassembly diff of two owner_live ELFs (V2 recorder evidence).

Usage: elf_function_diff.py BASE.elf HEAD.elf TOOL_PREFIX
Instruction addresses are dropped; call/branch targets compare by symbol name;
every l32r compares by the VALUE of its literal, resolved to a sized symbol
(+offset), a C string (its content) or the raw constant. Prints LOAD section
sizes and every function that is new, removed or whose normalized body
differs. A function not listed is instruction-identical in both images.
"""
import bisect, re, struct, subprocess, sys
base, head, prefix = sys.argv[1:4]
SECTIONS = ['.iram0.vectors', '.iram0.text', '.flash.text']


class Img:
    def __init__(s, elf):
        s.blob = open(elf, 'rb').read(); s.secs = []
        lines = subprocess.check_output([prefix + 'objdump', '-h', elf], text=True).splitlines()
        for i, l in enumerate(lines[:-1]):
            m = re.match(r'\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)', l)
            if m and 'LOAD' in lines[i + 1]:
                s.secs.append((int(m.group(3), 16), int(m.group(2), 16), int(m.group(4), 16), m.group(1)))
        s.syms = []
        for l in subprocess.check_output([prefix + 'nm', '-C', '-S', '-n', '--defined-only', elf], text=True).splitlines():
            m = re.match(r'([0-9a-f]+) ([0-9a-f]+) \w (.+)$', l)
            if m and int(m.group(2), 16): s.syms.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
        s.starts = [x[0] for x in s.syms]
    def read(s, addr, n):
        for vma, size, off, _ in s.secs:
            if vma <= addr < vma + size: return s.blob[off + addr - vma: off + addr - vma + n]
        return None
    def value(s, addr):
        b = s.read(addr, 4)
        if b is None or len(b) < 4: return 'NOLIT'
        v = struct.unpack('<I', b)[0]
        i = bisect.bisect_right(s.starts, v) - 1
        if i >= 0 and s.syms[i][0] <= v < s.syms[i][0] + s.syms[i][1]:
            return '%s+%d' % (s.syms[i][2], v - s.syms[i][0])
        if s.read(v, 1) is not None:
            raw = s.read(v, 96) or b''
            z = raw.split(b'\0')[0]
            if len(z) >= 2 and all(32 <= c < 127 or c in (9, 10) for c in z): return 'STR' + repr(z)
            return 'IMG'
        return '#%x' % v
    def funcs(s, elf):
        out, name = {}, None
        for sec in SECTIONS:
            text = subprocess.check_output([prefix + 'objdump', '-d', '-C', '--no-show-raw-insn', '-j', sec, elf], text=True)
            for line in text.splitlines():
                m = re.match(r'^[0-9a-f]{8} <(.+)>:$', line)
                if m: name = m.group(1); out.setdefault(name, []); continue
                m = re.match(r'^\s*[0-9a-f]+:\s+(.*)$', line)
                if not m or name is None: continue
                ins = m.group(1)
                lit = re.match(r'(l32r\s+\w+),\s*([0-9a-f]+)', ins)
                if lit: ins = lit.group(1) + ', ' + s.value(int(lit.group(2), 16))
                else:
                    ins = re.sub(r'\b[0-9a-f]{8} <([^>+]+)(\+0x[0-9a-f]+)?>',
                                 lambda m: '<%s%s>' % (m.group(1), m.group(2) if m.group(1) == name else ''), ins)
                out[name].append(ins)
        return out


def sizes(elf):
    res = {}
    for line in subprocess.check_output([prefix + 'size', '-A', elf], text=True).splitlines():
        p = line.split()
        if len(p) >= 2 and p[0] in SECTIONS + ['.dram0.data', '.dram0.bss', '.flash.rodata', '.flash.appdesc']:
            res[p[0]] = int(p[1])
    return res


sb, sh = sizes(base), sizes(head)
for k in sb: print('%-15s base=%8d head=%8d delta=%+d' % (k, sb[k], sh.get(k, 0), sh.get(k, 0) - sb[k]))
fb, fh = Img(base).funcs(base), Img(head).funcs(head)
new = sorted(set(fh) - set(fb)); gone = sorted(set(fb) - set(fh))
changed = sorted(n for n in set(fb) & set(fh) if fb[n] != fh[n])
print('functions: base=%d head=%d new=%d removed=%d changed=%d identical=%d' % (
    len(fb), len(fh), len(new), len(gone), len(changed), len(set(fb) & set(fh)) - len(changed)))
for n in new: print('NEW     ', n)
for n in gone: print('REMOVED ', n)
for n in changed: print('CHANGED ', n, '(%d -> %d insns)' % (len(fb[n]), len(fh[n])))
