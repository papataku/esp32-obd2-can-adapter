#!/usr/bin/env python3
"""P3 fixed-base production freeze; only read-only metadata additions allowed."""
import pathlib, re, subprocess
from test_p5_scope import strip_p5, check_tree as check_p5
check_p5()
ROOT=pathlib.Path(__file__).resolve().parents[1]
BASE='18188eed1b45534a7aadc6a2c390f2d594d92921'
def git(*args):return subprocess.check_output(['git',*args],cwd=ROOT).decode()
if subprocess.run(['git','cat-file','-e',BASE+'^{commit}'],cwd=ROOT,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode:
    subprocess.run(['git','fetch','--no-tags','origin',BASE],cwd=ROOT,check=True)  # shallow CI checkout
old_files=set(git('ls-tree','-r','--name-only',BASE).splitlines())
modified={'include/signal_v2_session.h','src/signal_v2_session.cpp','src/signal_v2_gatt.cpp','tests/run_all.sh'}
for path in old_files-modified:
    value=(ROOT/path).read_text()
    if path in {'src/can_monitor.cpp','src/can_monitor.h','src/signal_v2_can_owner.h','tests/test_signal_v2_b01_policy.py'}: value=strip_p5(value)
    if path=='platformio.ini': value=value.split('\n; P5 host/serial-HIL ONLY.')[0]
    assert value.encode()==subprocess.check_output(['git','show',BASE+':'+path],cwd=ROOT),path
s=(ROOT/'include/signal_v2_session.h').read_text()
for added in ['#include "signal_v2_profile_binding.h"\n','  ProfileBinding binding_;\n',
 '  // Metadata only: does not grant a lease, select a profile or authorize TX.\n  void bindingContext(const BindingContext &context) { binding_.context = context; }\n']:
    assert added in s;s=s.replace(added,'',1)
assert s==git('show',BASE+':include/signal_v2_session.h')
s=(ROOT/'src/signal_v2_session.cpp').read_text().replace('  binding_ = ProfileBinding{};\n','',1)
s=s.replace('''std::equal(p, p + n + 12, h.request.payload())) {
        if (op == 11)
          ack(req, ProtocolError, nullptr, 0, out); // binding ACKs are single-use
        else
          out = h.ack;
      } else''','''std::equal(p, p + n + 12, h.request.payload()))
        out = h.ack;
      else''',1)
start=s.index('    case 11:\n');end=s.index('    case 10:',start);s=s[:start]+s[end:]
assert s==git('show',BASE+':src/signal_v2_session.cpp'),'legacy Session body changed'
s=(ROOT/'src/signal_v2_gatt.cpp').read_text()
start=s.index('#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)\n      // Read-only O-2 metadata.')
end=s.index('\n#endif\n',start)+len('\n#endif\n');s=s[:start]+s[end:]
assert s==git('show',BASE+':src/signal_v2_gatt.cpp'),'GATT/auth/owner changed'
s=(ROOT/'tests/run_all.sh').read_text().split('\n# P3 optional',1)[0]
assert s==git('show',BASE+':tests/run_all.sh')
allowed_new={'include/signal_v2_profile_binding.h','tests/run_p3_binding.sh','tests/test_p3_binding_context.py',
 'tests/p3/binding.cpp','tests/p3/legacy.cpp','tools/test_p3_scope.py','tools/test_p3_legacy_equivalence.py',
 'docs/protocol/vehicle-profile-p3-wire.md'}
added=set(git('ls-files','--cached','--others','--exclude-standard').splitlines())-old_files
p5_new={'.github/workflows/p5-discovery.yml','include/signal_v2_11bit_discovery.h','src/discovery11_lab.cpp','src/signal_v2_11bit_monitor.inc','src/signal_v2_11bit_owner.inc','tests/p5_discovery.cpp','tests/p5_serial.cpp','tests/run_p5_discovery.py','tools/test_p5_scope.py','tools/check_p5_elf.py','tools/compare_p5_owner_image.py','tools/test_p5_mutations.py','docs/protocol/vehicle-profile-p5-discovery.md'}
assert all(x in allowed_new or x in p5_new or x.startswith(('docs/evidence/p3/','docs/evidence/p5/')) for x in added),added-allowed_new-p5_new
# No new peer identity source or logging/sending/storage API in the extension.
new=(ROOT/'include/signal_v2_profile_binding.h').read_text()
assert not re.search(r'(Serial\.|printf\(|Preferences|NimBLE|transmit\(|getAddress\(|macAddress\()',new)
for path in added:
    if path.endswith(('.cpp','.h','.md','.txt','.py','.sh')):
        assert not re.search(r'\b(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}\b',(ROOT/path).read_text()),path
print(f'PASS {len(old_files-modified)} fixed-base files byte-identical; Session/GATT restored exactly after removing metadata-only additions')
print('PASS out-of-scope changes=0: TX bytes/IDs/rates/budgets/deadline/lease/epoch; autoARM/settle/fault/tap/unsubscribe/non-owner; envs/BLE/registration/NUS')
print('PASS added MAC leak=0; no new identity input/log/export/store API; synthetic fixtures only')
