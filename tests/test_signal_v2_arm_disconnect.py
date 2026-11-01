"""Deterministic production GATT interleavings; no BLE/device access."""
import pathlib
import subprocess
import tempfile
import unittest

from test_signal_v2_b01_policy import ROOT, OWNER, HIL, flags_by_env


class ArmDisconnect(unittest.TestCase):
    def test_production_arm_disconnect_and_reconnect(self):
        source = (ROOT / "src/signal_v2_gatt.cpp").read_text()
        arm = '  if(!std::strcmp(cmd,"SIGV2 CAN ARM")) {'
        arm += source.split(arm, 1)[1].split('  // PR-C2 stage S1', 1)[0]
        disconnect = 'void disconnected(uint16_t handle) {'
        disconnect += source.split(disconnect, 1)[1].split('void gap(', 1)[0]
        fixture = (ROOT / 'tests/arm_disconnect_fixture.cpp.in').read_text()
        _, flags = flags_by_env()
        with tempfile.TemporaryDirectory() as d:
            cpp = pathlib.Path(d) / 'arm.cpp'
            cpp.write_text(fixture.replace('@ARM@', arm).replace('@DISCONNECT@', disconnect))
            for env in (OWNER, HIL):
                exe = pathlib.Path(d) / 'arm'
                result = subprocess.run([
                    'c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    *[f for f in flags[env] if f.startswith(('-D', '-U'))],
                    '-Itests/host_stubs', '-Iinclude', '-Isrc', str(cpp),
                    'src/diagnostic_policy.cpp', 'src/diagnostic_transaction.cpp',
                    'src/isotp_reassembler.cpp', '-o', str(exe)],
                    cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                for race in range(4):
                    for check in ('armed', 'reconnect'):
                        with self.subTest(env=env, race=race, check=check):
                            result = subprocess.run([str(exe), str(race), check],
                                                    capture_output=True, text=True)
                            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                            print(env, result.stdout.strip())


if __name__ == '__main__':
    unittest.main()
