#!/usr/bin/env python3
"""Read-only gate for app-only passive flash on the known 8MiB factory layout."""
import argparse
import hashlib
import json
from pathlib import Path
from signal_v2_flash_check import inspect

PARTITIONS_SHA256 = '7891dd9f69f2da4a49b4ed63ec4bd1651f9258d0f0787249e3f7bae65591645f'

def check(backup, firmware, partitions, bootloader):
    table = Path(partitions).read_bytes()
    if len(table) != 0xC00 or hashlib.sha256(table).hexdigest() != PARTITIONS_SHA256:
        raise ValueError('unknown candidate partition table: STOP')
    raw = Path(backup).read_bytes()
    boot = Path(bootloader).read_bytes()
    if not 0 < len(boot) <= 0x8000 or raw[:len(boot)] != boot:
        raise ValueError('installed bootloader differs: STOP; never replace implicitly')
    if raw[0x8000:0x9000] != table + b'\xff' * 0x400:
        raise ValueError('installed partition sector differs: STOP; never migrate')
    result = inspect(backup, firmware, 'm5dial')
    apps = [r for r in result['partitions'] if r['type'] == 0]
    if len(apps) != 1 or apps[0]['subtype'] != 0 or apps[0]['flags']:
        raise ValueError('only the known unencrypted single factory app is supported')
    result['profile'] = 'm5dial_signal_v2_can_29bit_passive'
    return result

if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--backup', required=True)
    p.add_argument('--firmware', required=True)
    p.add_argument('--partitions', required=True)
    p.add_argument('--bootloader', required=True)
    a = p.parse_args()
    print(json.dumps(check(a.backup, a.firmware, a.partitions, a.bootloader), indent=2))
