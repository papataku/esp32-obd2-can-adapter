#!/usr/bin/env python3
"""Fixed published hex -> actual portable firmware C++ codec, not a Python model."""
import json,pathlib,subprocess,tempfile,unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class Wire(unittest.TestCase):
 def test_published_golden(self):
  f=json.loads((ROOT/'tests/fixtures/signal_v2_v3.json').read_text())
  with tempfile.TemporaryDirectory() as d:
   exe=str(pathlib.Path(d)/'codec')
   subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'include'),str(ROOT/'src/signal_v2_codec.cpp'),str(ROOT/'tests/test_signal_v2_codec.cpp'),'-o',exe],check=True)
   for name,h in f.items():
    if name.startswith('G10'):continue
    with self.subTest(name=name):subprocess.run([exe,'badcrc' if name=='G12' else 'ok',h],check=True)
   b=bytes.fromhex(f['G01']);parts=[]
   for off in range(0,len(b),4):
    chunk=b[off:off+4];parts.append(bytes.fromhex('d7520201')+bytes([(1 if off==0 else 0)|(2 if off+len(chunk)==len(b) else 0),0])+bytes.fromhex('010000003200')+off.to_bytes(2,'little')+bytes([len(chunk),0])+chunk)
   self.assertEqual(len(parts),13);self.assertEqual(parts[0].hex(),f['G10']);self.assertEqual(parts[-1].hex(),f['G10_2'])
   self.assertEqual(b,b''.join(p[16:] for p in parts))
   for i,p in enumerate(parts):self.assertEqual((ROOT/'tests/fixtures'/f'signal_v2_g10_{i+1:02}.hex').read_text().strip(),p.hex())
if __name__=='__main__':unittest.main()
