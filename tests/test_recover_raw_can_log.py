#!/usr/bin/env python3
"""Golden v1 CAN RAW codec and strict USB framing regression."""
import importlib.util
import struct
import tempfile
import zlib
from pathlib import Path

MODULE = Path(__file__).resolve().parents[1] / 'tools' / 'recover_raw_can_log.py'
spec = importlib.util.spec_from_file_location('raw_can_recover', MODULE)
mod = importlib.util.module_from_spec(spec)
import sys
sys.modules[spec.name] = mod
spec.loader.exec_module(mod)


def uleb(x):
    out = bytearray()
    while True:
        b = x & 127
        x >>= 7
        out.append(b | (128 if x else 0))
        if not x:
            return bytes(out)


def sample_block():
    # frame1 = 11bit RX 0x7E8, 8 bytes. frame2 = same ID+data,
    # 10ms later. frame3 = TX_SUBMITTED 29bit 0x18DB33F1, DLC 2.
    payload = bytearray()
    payload += b'\0\0' + bytes([0, 8]) + uleb(0x7E8) + bytes.fromhex('04410C0012000000')
    payload += uleb(1) + uleb(10000) + bytes([0x60, 8])
    payload += uleb(1) + uleb(30000) + bytes([0x05, 2]) + uleb(0x18DB33F1) + bytes.fromhex('010C')
    first = bytearray(32)
    first[:4] = b'MCAN'
    first[4] = 1
    struct.pack_into('<HIQI', first, 6, 3, 50, 123456789, len(payload))
    struct.pack_into('<I', first, 24, zlib.crc32(first[:24] + payload) & 0xFFFFFFFF)
    return bytes(first + payload)


def run():
    raw = sample_block()
    events = mod.decode(raw)
    assert len(events) == 3
    assert [r.sequence for r in events] == [50, 51, 52]
    assert [r.timestamp_us for r in events] == [123456789, 123466789, 123496789]
    assert events[0].can_id == 0x7E8 and events[1].data == events[0].data
    assert events[2].can_id == 0x18DB33F1 and events[2].extended
    assert events[2].direction == 'TX_SUBMITTED'
    damaged = bytearray(raw)
    damaged[-1] ^= 1
    try:
        mod.decode(bytes(damaged))
        raise AssertionError('CRC damage accepted')
    except mod.RawCanError:
        pass

    # The same format is emitted by serial_protocol.cpp in 32-byte hex lines.
    lines = [f'#CANLOG,BLOCK,index=0,type=1,length={len(raw)}']
    for offset in range(0, len(raw), 32):
        lines.append(f'#CANLOG,DATA,index=0,offset={offset},hex={raw[offset:offset+32].hex().upper()}')
    lines += ['#CANLOG,BLOCKEND,index=0',
              '#CANLOG,BLOCK,index=1,type=2,length=0',
              '#CANLOG,BLOCKEND,index=1']
    chunks, complete = mod.recover_usb('\n'.join(lines))
    assert complete and chunks[0] == (1, raw)
    with tempfile.TemporaryDirectory() as directory:
        result = mod.export(chunks, Path(directory), complete)
        assert result['events'] == 3
        assert result['observed_sequence_gaps'] == 0
        assert (Path(directory) / 'can_frames.csv').is_file()
    try:
        mod.recover_usb('\n'.join(lines[:2] + lines[3:]))
        raise AssertionError('missing USB chunk silently accepted')
    except mod.RawCanError:
        pass
    print('CANLOG Python decoder and USB recovery PASS')


if __name__ == '__main__':
    run()
