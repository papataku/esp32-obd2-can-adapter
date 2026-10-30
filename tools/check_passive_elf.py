#!/usr/bin/env python3
"""Verify the actual linked image, rather than software TX counters."""
import argparse
import hashlib
from pathlib import Path
import subprocess
p = argparse.ArgumentParser()
p.add_argument('elf')
p.add_argument('--nm', required=True)
a = p.parse_args()
symbols = subprocess.check_output([a.nm, '--defined-only', '-C', a.elf], text=True)
for name in ('twai_transmit', 'twai_initiate_recovery', 'CanMonitor::',
             'signal_v2::', 'NimBLE', 'esp_ble_', 'SerialProtocol::',
             'M5GFX::autodetect', 'M5Unified::',
             'TwoWire::begin(', 'TwoWire::setPins(',
             'i2cInit', 'i2c_driver_install', 'i2c_param_config',
             'i2c_new_master_bus', 'lgfx::v1::i2c::init('):
    if name in symbols:
        raise SystemExit('FORBIDDEN passive ELF symbol: ' + name)
for name in ('twai_driver_install', 'twai_start', 'twai_stop',
             'twai_driver_uninstall', 'twai_receive', 'twai_get_status_info'):
    if not any(line.split()[-1] == name for line in symbols.splitlines()):
        raise SystemExit('MISSING passive ELF symbol: ' + name)
print('PASS: RX present; TX/ARM/lease/BLE and I2C pin-initialization symbols absent')
print('ELF SHA256: ' + hashlib.sha256(Path(a.elf).read_bytes()).hexdigest())
