#!/usr/bin/env python3
"""Exercise linked-symbol deny checks, including CI wiring to V2-1 ELF."""
import pathlib, subprocess, sys, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class LinkedPolicy(unittest.TestCase):
    def check_symbols(self, symbols):
        with tempfile.TemporaryDirectory() as d:
            elf=pathlib.Path(d)/'fixture.elf';elf.write_bytes(b'linked ELF fixture')
            nm=pathlib.Path(d)/'nm'
            nm.write_text('#!/bin/sh\ncat <<\'SYMBOLS\'\n'+symbols+'\nSYMBOLS\n')
            nm.chmod(0o700)
            return subprocess.run([sys.executable,str(ROOT/'tools/check_signal_v2_elf.py'),
                str(elf),'--nm',str(nm)],capture_output=True,text=True)
    def test_defined_tx_and_init_rejected(self):
        for symbol in ('twai_transmit','twai_driver_install','twai_start'):
            with self.subTest(symbol=symbol):
                result=self.check_symbols('00001234 T '+symbol)
                self.assertNotEqual(result.returncode,0)
                self.assertIn('FORBIDDEN',result.stderr)
    def test_safe_symbols_pass(self):
        self.assertEqual(self.check_symbols('00001234 T safe_function').returncode,0)
    def test_ci_checks_v21_linked_elf(self):
        policy=(ROOT/'.github/workflows/signal-v2-lab.yml').read_text()
        self.assertIn('check_signal_v2_elf.py .pio/build/m5dial_signal_v2_can_disabled_lab/firmware.elf',policy)
if __name__=='__main__': unittest.main()
