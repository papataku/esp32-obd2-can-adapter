#!/usr/bin/env python3
"""P2 fixed-base scope audit; run for this slice, not unrelated future PRs."""
import hashlib
import pathlib
import subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
BASE='de635e9f628d46e26050019c15cf337482789276'
ALLOWED={'include/signal_v2_can_samples.h','include/signal_v2_discovery_profile.h',
         'src/signal_v2_can_budget.h','src/signal_v2_can_owner.h','tests/run_all.sh'}
NEW={'include/signal_v2_vehicle_profile.h','tests/p2/profile.cpp','tests/p2/serial_equivalence.cpp',
     'tools/test_p2_profile_equivalence.py','tools/test_p2_profile_scope.py','tests/run_p2_profile.sh',
     'docs/protocol/vehicle-profile-p2.md'}

def git(*args):return subprocess.check_output(['git',*args],cwd=ROOT)
count=0
for name in git('ls-tree','-r','--name-only',BASE).decode().splitlines():
    old=git('show',BASE+':'+name)
    if name not in ALLOWED:
        assert (ROOT/name).read_bytes()==old,'out-of-scope change: '+name
        count+=1
for name in git('ls-files','--cached','--others','--exclude-standard').decode().splitlines():
    assert name in set(git('ls-tree','-r','--name-only',BASE).decode().splitlines()) or name in NEW or name.startswith('docs/evidence/p2/'), 'unexpected added file: '+name
# Fields are tested for exact values in C++11; evidence digest is audit metadata.
source=git('show',BASE+':include/signal_v2_discovery_profile.h')
assert hashlib.sha256(source).hexdigest() in (ROOT/'include/signal_v2_vehicle_profile.h').read_text()
# Freeze owner authorization/epoch handling (all data edits occur elsewhere).
old=git('show',BASE+':src/signal_v2_can_owner.h').decode()
new=(ROOT/'src/signal_v2_can_owner.h').read_text()
for start,end in [('public:', '    bool has_demand=false;'),('  void stop(', '  // One parseable record')]:
    assert old.split(start,1)[1].split(end,1)[0]==new.split(start,1)[1].split(end,1)[0]
print(f'PASS {count} fixed-base files byte-identical; wire/PDU, driver, auth/GATT/UI, BLE/NUS, envs, lease/epoch unchanged')
print('PASS P2 file allowlist, provenance digest, owner authorization blocks; no external-repo files')
