#!/usr/bin/env python3
"""Check compiler frame budgets; runtime HWM and library paths still need QA."""
import sys
from pathlib import Path

BUDGETS = {
    '::bleAppTask(': 128,
    'm5can::DualBleElm::poll(': 128,
    'm5can::DualBleElm::event(': 1536,
    'm5can::DualBleElm::onLine(': 1280,
    'm5can::DualBleElm::onResult(': 2048,
    'm5can::BleElmTransport::writeTo(': 256,
}
for env in sys.argv[1:]:
    frames = {}
    files = list((Path('.pio/build') / env / 'src').glob('*.su'))
    assert files, f'{env}: compiler stack files missing'
    for p in files:
        for line in p.read_text().splitlines():
            fields = line.split('\t')
            if len(fields) != 3:
                continue
            name, size, kind = fields
            for key, budget in BUDGETS.items():
                if key in name:
                    assert kind == 'static', f'{env}: unbounded frame {name}: {kind}'
                    frames[key] = max(frames.get(key, 0), int(size))
                    assert int(size) <= budget, f'{env}: {name} {size}>{budget}'
    if 'dual_ble' in env:
        assert set(frames) == set(BUDGETS), f'{env}: missing functions {set(BUDGETS)-set(frames)}'
        # Conservative selected frames, including mutually exclusive paths.
        # Retain 2048 bytes for nested library calls; HWM is the runtime gate.
        estimate = sum(frames.values()) + 2048
        assert estimate <= 8192, f'{env}: selected frame sum plus reserve {estimate}>8192'
        print(f'PASS {env}: frames {frames}; selected sum+reserve={estimate}/8192')
    else:
        print(f'PASS {env}: {len(files)} compiler frame files present (no dual worker)')
