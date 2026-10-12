#!/usr/bin/env python3
"""Check compiler frame budgets; runtime HWM and library paths still need QA."""
import sys
from pathlib import Path

BUDGETS = {
    'm5can::CanMonitor::rxTask(': 512,
    'm5can::CanMonitor::performQuery(': 512,
    '::bleAppTask(': 128,
    'm5can::DualBleElm::poll(': 128,
    'm5can::DualBleElm::event(': 1536,
    'm5can::DualBleElm::onLine(': 1280,
    'm5can::DualBleElm::onResult(': 2048,
    'm5can::BleElmTransport::writeTo(': 256,
    'm5can::DriveLog::addEvent(': 192,
    'm5can::DriveLog::outcome(': 128,
    'm5can::DriveLog::submitted(': 128,
}
# Additional coordinator frames: these may be inlined and accounted for in
# their caller. When emitted as their own static frames, enforce limits too.
EXTRA_BUDGETS = {
    'm5can::DualBleElm::dispatch(': 1024,
    'm5can::DualBleElm::advanceBatch(': 768,
    'm5can::DualBleElm::finishSingle(': 512,
    'm5can::DualBleElm::cacheSafePartialGroup(': 512,
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
            for key, budget in {**BUDGETS, **EXTRA_BUDGETS}.items():
                if key in name:
                    assert kind == 'static', f'{env}: unbounded frame {name}: {kind}'
                    frames[key] = max(frames.get(key, 0), int(size))
                    assert int(size) <= budget, f'{env}: {name} {size}>{budget}'
    if 'dual_ble' in env:
        assert set(BUDGETS).issubset(frames), f'{env}: missing functions {set(BUDGETS)-set(frames)}'
        # Stack depth depends on one call chain, NOT the sum of every
        # mutually exclusive session callback. Conservatively bound paths:
        # bleAppTask -> poll -> event -> onLine -> advanceBatch
        # bleAppTask -> poll -> onResult -> finishSingle -> advanceBatch
        # bleAppTask -> poll -> dispatch -> finishSingle -> advanceBatch
        # ESP32 NimBLE / Serial / C++ nested-library reserve is 2048 bytes.
        # Out-of-line optional frames count here; inlined frames are part
        # of their parent's compiler .su value and need not be added twice.
        def sz(name):
            return frames.get(name, 0)
        event_path = sz('m5can::DualBleElm::event(') + max(
            sz('m5can::DualBleElm::onLine(') +
            sz('m5can::DualBleElm::advanceBatch('),
            sz('m5can::DriveLog::addEvent('))
        result_path = sz('m5can::DualBleElm::onResult(') + max(
            sz('m5can::DualBleElm::cacheSafePartialGroup('),
            sz('m5can::DriveLog::outcome(') + sz('m5can::DriveLog::addEvent('),
            sz('m5can::DualBleElm::finishSingle(') +
            sz('m5can::DualBleElm::advanceBatch('))
        dispatch_path = sz('m5can::DualBleElm::dispatch(') + (
            sz('m5can::DualBleElm::finishSingle(') +
            sz('m5can::DualBleElm::advanceBatch('))
        estimate = (sz('::bleAppTask(') +
                    sz('m5can::DualBleElm::poll(') +
                    max(event_path,result_path,dispatch_path) +
                    sz('m5can::BleElmTransport::writeTo(') + 2048)
        assert estimate <= 8192, f'{env}: worst BLE chain+reserve {estimate}>8192'
        print(f'PASS {env}: frames {frames}; worst chain+reserve={estimate}/8192')
    else:
        assert all(k in frames for k in BUDGETS if 'CanMonitor' in k), f'{env}: CAN frame files missing'
        print(f'PASS {env}: CAN frames {frames} (no dual worker)')
