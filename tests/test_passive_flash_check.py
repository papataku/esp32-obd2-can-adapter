#!/usr/bin/env python3
import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from passive_flash_check import check, PARTITIONS_SHA256

def image():
    data = bytearray(24); data[0:2] = bytes([0xE9, 1])
    data += struct.pack('<II', 0x3FC80000, 4) + b'test'
    data += b'\0' * ((len(data)+16)//16*16-len(data))
    return data + hashlib.sha256(data).digest()

def table():
    data = b''
    for name,typ,sub,off,size in (
        ('nvs',1,2,0x9000,0x5000),('otadata',1,0,0xE000,0x2000),
        ('factory',0,0,0x10000,0x200000),('canlog',1,0x40,0x210000,0x460000),
        ('spiffs',1,0x82,0x670000,0x180000),('coredump',1,3,0x7F0000,0x10000)):
        data += struct.pack('<HBBII16sI',0x50AA,typ,sub,off,size,name.encode(),0)
    data += b'\xEB\xEB' + b'\xFF'*14 + hashlib.md5(data).digest()
    return data.ljust(0xC00,b'\xFF')

class FlashGate(unittest.TestCase):
    def test_exact_layout_only_and_images_validate(self):
        t=table()
        self.assertEqual(hashlib.sha256(t).hexdigest(),PARTITIONS_SHA256)
        raw=bytearray(b'\xFF'*0x800000); raw[0x8000:0x8C00]=t
        fw=image(); raw[0x10000:0x10000+len(fw)]=fw
        with tempfile.TemporaryDirectory() as d:
            d=Path(d); b=d/'backup'; f=d/'firmware'; p=d/'partitions'; boot=d/'bootloader'
            boot.write_bytes(raw[:0x100])
            b.write_bytes(raw); f.write_bytes(fw); p.write_bytes(t)
            self.assertEqual(check(b,f,p,boot)['app']['offset'],0x10000)
            raw[0x8FFF]=0; b.write_bytes(raw)
            with self.assertRaisesRegex(ValueError,'partition sector differs'): check(b,f,p,boot)
            raw[0x8FFF]=255; b.write_bytes(raw)
            p.write_bytes(t[:-1]+b'\0')
            with self.assertRaisesRegex(ValueError,'unknown candidate'): check(b,f,p,boot)
            p.write_bytes(t)
            boot.write_bytes(b'unknown bootloader')
            with self.assertRaisesRegex(ValueError,'bootloader differs'): check(b,f,p,boot)
            boot.write_bytes(raw[:0x100])
            p.write_bytes(t); f.write_bytes(fw[:-1]+bytes([fw[-1]^1]))
            with self.assertRaisesRegex(ValueError,'SHA mismatch'): check(b,f,p,boot)
            f.write_bytes(fw+b'\xff'*0x200000)
            with self.assertRaisesRegex(ValueError,'exceeds'): check(b,f,p,boot)
            f.write_bytes(fw); b.write_bytes(raw[:-1])
            with self.assertRaisesRegex(ValueError,'flash size'): check(b,f,p,boot)
if __name__=='__main__': unittest.main()
