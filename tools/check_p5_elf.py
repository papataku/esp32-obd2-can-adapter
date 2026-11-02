#!/usr/bin/env python3
"""Check actual lab/legacy linked ELFs. Never opens a device."""
import argparse, configparser, hashlib, pathlib, re, subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--nm',required=True);p.add_argument('--all',action='store_true');a=p.parse_args()
LAB='m5dial_signal_v2_can_11bit_discovery_lab'
OWNER='m5dial_signal_v2_can_29bit_5hz_owner_live'
c=configparser.ConfigParser(interpolation=None);c.read(ROOT/'platformio.ini')
names=[s[4:] for s in c.sections() if s.startswith('env:')] if a.all else [OWNER,LAB]
for name in names:
    elf=ROOT/'.pio/build'/name/'firmware.elf'
    symbols=subprocess.check_output([a.nm,'-C','--defined-only',str(elf)],text=True)
    count=sum(bool(re.search(r'Discovery11|discovery11_|performDiscovery11|armDiscovery11|tickDiscovery11',line)) for line in symbols.splitlines())
    assert (count>0 if name==LAB else count==0),(name,count)
    if name==LAB:
        assert not re.search(r'\b(twai_\w+)\s*$',symbols,re.M),'physical TWAI linked'
        assert not re.search(r'M5GFX::|M5Unified::|M5Dial::|NimBLE|signal_v2::Session::',symbols),'unexpected board/BLE/wire code'
    print('PASS',name,'discovery_symbols='+str(count),'firmware.bin SHA256='+hashlib.sha256(elf.with_suffix('.bin').read_bytes()).hexdigest())
