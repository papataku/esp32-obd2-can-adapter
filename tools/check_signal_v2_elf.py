#!/usr/bin/env python3
"""Fail a lab ELF containing a physical TX or CAN-driver-init entry point."""
import argparse,subprocess,hashlib,pathlib
p=argparse.ArgumentParser();p.add_argument('elf');p.add_argument('--nm',required=True);p.add_argument('--hil',action='store_true');a=p.parse_args()
s=subprocess.check_output([a.nm,'--defined-only',a.elf],text=True)
for line in s.splitlines():
 name=line.split()[-1]
 if name.startswith('twai_'):raise SystemExit('FORBIDDEN in CAN-disabled ELF: '+name)
if a.hil:
 demangled=subprocess.check_output([a.nm,'-C','--defined-only',a.elf],text=True)
 for forbidden in ('M5GFX::','M5Unified::','M5Dial::'):
  if forbidden in demangled:raise SystemExit('FORBIDDEN in headless HIL ELF: '+forbidden)
print('PASS: physical TWAI TX and driver initialization symbols absent')
print('ELF SHA256: '+hashlib.sha256(pathlib.Path(a.elf).read_bytes()).hexdigest())
