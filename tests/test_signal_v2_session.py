#!/usr/bin/env python3
import json,pathlib,subprocess,tempfile,unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class Session(unittest.TestCase):
 def test_firmware_state_machine(self):
  f=json.loads((ROOT/'tests/fixtures/signal_v2_v3.json').read_text())
  with tempfile.TemporaryDirectory() as d:
   exe=str(pathlib.Path(d)/'session')
   subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(ROOT/'src/signal_v2_codec.cpp'),str(ROOT/'src/signal_v2_session.cpp'),str(ROOT/'tests/test_signal_v2_session.cpp'),'-o',exe],check=True)
   subprocess.run([exe,*[f[x] for x in ('G01','G02','G03','G04','G11','G11_2')]],check=True)
if __name__=='__main__':unittest.main()
