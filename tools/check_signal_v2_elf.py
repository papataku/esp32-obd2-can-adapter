#!/usr/bin/env python3
"""Fail a lab ELF containing a physical TX or CAN-driver-init entry point."""
import argparse,subprocess,hashlib,pathlib
p=argparse.ArgumentParser();p.add_argument('elf');p.add_argument('--nm',required=True);a=p.parse_args()
s=subprocess.check_output([a.nm,'--defined-only',a.elf],text=True)
for name in ('twai_transmit','twai_driver_install','twai_start'):
 if any(line.split()[-1]==name for line in s.splitlines()):raise SystemExit('FORBIDDEN in CAN-disabled ELF: '+name)
print('PASS: physical TWAI TX and driver initialization symbols absent')
print('ELF SHA256: '+hashlib.sha256(pathlib.Path(a.elf).read_bytes()).hexdigest())
