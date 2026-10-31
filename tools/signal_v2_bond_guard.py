"""NimBLE 1.4.3 deletes an existing bond on repeat-pairing by default.
Patch only this env's local dependency copy; fail closed if upstream changes.
Must run after dependency installation (PlatformIO extra_scripts).
"""
Import('env')
from pathlib import Path
p=Path(env.subst('$PROJECT_LIBDEPS_DIR'))/env.subst('$PIOENV')/'NimBLE-Arduino/src/NimBLEServer.cpp'
if not p.exists():
 raise RuntimeError('Signal V2 requires installed pinned NimBLE-Arduino 1.4.3')
s=p.read_text()
old='            ble_store_util_delete_peer(&desc.peer_id_addr);'
new='            return BLE_GAP_REPEAT_PAIRING_IGNORE; // Signal V2: preserve owner bonds'
if old in s:s=s.replace(old,new);p.write_text(s)
elif new not in s:raise RuntimeError('Signal V2 bond guard: upstream implementation changed')

p=p.with_name('NimBLEClient.cpp')
s=p.read_text()
old='                    ble_store_util_delete_peer(&desc.peer_id_addr);'
new='                    /* Signal V2: key mismatch fails closed; preserve existing bond. */'
if old in s:s=s.replace(old,new);p.write_text(s)
elif new not in s:raise RuntimeError('Signal V2 client bond guard: upstream implementation changed')
