"""Execute production FIFO admission/dispatch and ACK-confirm code with real Session.

Only FreeRTOS/NimBLE IO is stubbed. No model of Control or lease semantics.
SIGNAL_V2_GATT_SOURCE allows the same regressions to run against the base source.
"""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class ControlDuringAck(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = pathlib.Path(os.environ.get(
            'SIGNAL_V2_GATT_SOURCE', ROOT / 'src/signal_v2_gatt.cpp')).read_text()
        def block(start, end):
            return start + source.split(start, 1)[1].split(end, 1)[0]
        fixture = (ROOT / 'tests/control_ack_fixture.cpp.in').read_text()
        replacements = {
            '@DISPATCH@': source.split('  Incoming in;\n  if (xQueue', 1)[1],
            '@REVOKE@': block('// Sticky per-slot notification:', 'class Callbacks :') if '// Sticky per-slot notification:' in source else 'void revokeControlLink(uint16_t) {}',
            '@ENQUEUE@': block('        if (xQueueSend(inbox, &in, 0)', '\n        return;'),
            '@CONFIRM@': block('  if (p.waiting) {', '\n  bool isack ='),
        }
        # Keep the entire production tail, including stale-generation disposal.
        replacements['@DISPATCH@'] = ('  Incoming in;\n  if (xQueue' +
            replacements['@DISPATCH@'].split('\n} // namespace signal_v2', 1)[0])
        golden = json.loads((ROOT / 'tests/fixtures/signal_v2_v3.json').read_text())
        for name, key in [('HELLO', 'G01'), ('PROFILE', 'G03')]:
            replacements['@' + name + '@'] = ','.join(str(b) for b in bytes.fromhex(golden[key]))
        for key, value in replacements.items():
            fixture = fixture.replace(key, value)
        cls.tmp = tempfile.TemporaryDirectory()
        cpp = pathlib.Path(cls.tmp.name) / 'control.cpp'
        cpp.write_text(fixture)
        cls.exe = pathlib.Path(cls.tmp.name) / 'control'
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-Iinclude', str(cpp),
                        'src/signal_v2_codec.cpp', 'src/signal_v2_session.cpp',
                        '-o', str(cls.exe)], cwd=ROOT, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def scenario(self, name):
        result = subprocess.run([str(self.exe), name], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_keepalive_after_confirm(self): self.scenario('renew')
    def test_1700ms_ack_delay_with_1s_keepalive(self): self.scenario('delay')
    def test_fifo_order(self): self.scenario('fifo')
    def test_overflow_protocol_error_then_disconnect(self): self.scenario('overflow')
    def test_duplicate_rid_does_not_renew(self): self.scenario('duplicate')
    def test_ack_timeout_still_disconnects(self): self.scenario('timeout')
    def test_stale_generation_does_not_block_fifo(self): self.scenario('stale')
    def test_authorization_rechecked_after_wait(self): self.scenario('auth')


from test_signal_v2_control_overflow import ControlOverflow


if __name__ == '__main__':
    unittest.main()
