# P3 verification record (no devices)

Implementation/source commit: `5f88685939410bedc61edd7b5de69f2f2fcd5bc9`.
Immutable compatibility base: `18188eed1b45534a7aadc6a2c390f2d594d92921`.
P3 production-content SHA256 (path+bytes in order: binding header, Session
header, Session cpp, GATT cpp):
`4996d71643a73d9336e9e8976aa725ed51899390b4992e47a43146b44fcd1835`.
The later evidence-only commit does not change production code or tests.

| Command | Result |
| --- | --- |
| `bash tests/run_p3_binding.sh` | PASS: codec/MTU23 reassembly, host ASan/UBSan; production owner/monitor/serial shim on host. Unknown version, schema/revision/ID, old boot/session/epoch, replay including history eviction, switch and forged/lost evidence rejected; dynamic LIVE/demand/queued requests/driver TX=0 before and after ACK. |
| `python3 tests/test_p3_binding_context.py` | PASS: exact production GATT evidence projection; same-session owner only, no stale/future/invalid/wrong-route/fault evidence. |
| `python3 tools/test_p3_legacy_equivalence.py --output ../evidence/p3-legacy` | PASS: actual fixed-base and head separately compiled, CAN and DEMO full PDUs/state/lease/revision/demand equal. [Output](legacy-equivalence.txt). |
| `python3 tools/test_p3_legacy_equivalence.py --mutation-proof --output ../evidence/p3-mutation` | Expected FAIL, exit1. Temporary HELLO maxPDU 512→511 changes both DEMO/CAN bytes and CRC. [Failure log](mutation-fail.txt). Working source untouched; normal comparison passes. |
| `python3 tools/test_p3_scope.py` | PASS: 346 other fixed-base files unchanged; existing Session/GATT bodies reconstruct byte-exactly after removing only metadata additions. Out-of-scope changes=0. [Output](scope.txt). |
| `python3 tests/test_signal_v2_review_policy.py` | PASS: existing privacy tests 3/3; added-asset scanner and no identity/log/store API checks PASS, MAC leak=0. [Output](privacy.txt). |

No raw vehicle data or physical identity is used. `evidenceKind=1` is an
existing acquisition reference, not a fingerprint match. No dynamic activation
capability exists; valid tuple ACK is metadata receipt only. Existing legacy
PROFILE/TX authority remains independent and unchanged.

Physical serial HIL, devices/ports, RF/BLE/LCD/touch, flash/upload, bench CAN,
vehicle acceptance: **NOT RUN**. No physical CAN TX. GitHub CI is separate from
local checks and is not claimed as PASS here.

Initial local `run_all.sh` with Python3.9 stopped at the pre-existing
`zip(..., strict=True)` requirement; use Python3.11. During development the
new serial fixture was corrected to initialize the singleton serial shim once,
and the evidence-extraction test retained the production comment prefix.
No production policy or existing assertion was relaxed.

owner_live `m5dial_signal_v2_can_29bit_5hz_owner_live/firmware.bin` SHA256:
`8ca1beaa682e16e0791626b5a3f30aa797e57c3628906fe78d3609887dc94a92`.
Build-only synthetic provisioned peer/gate constants are inherited from the
existing P2 matrix helper; no real device registration/approval/partition
readback is asserted. All existing env flags and partitions are unchanged.

Final local checks:

- `PATH="$PWD/../tool-bin:$PATH" bash tests/run_all.sh` (**Python3.11**):
  **PASS exit0**, all existing host tests plus new P3 tests. P2 fixed-base
  host/serial-HIL TX equivalence passes at 5Hz/10Hz; exact budget boundary
  remains `1,5000` in base and head. [Final output and full-log hash](host-tail.txt).
- `python3 docs/evidence/p2/build_matrix.py`: **PASS exit0, 27/27**
  (26 platformio.ini envs + SmartRing lab). [Per-env result](build-matrix.txt).
  CAN-disabled, passive and serial-HIL linked-ELF policy hooks pass.
- `git diff --check`: PASS. Source mutation was only in a temporary directory;
  normal fixed-base legacy equivalence passes after mutation proof.

Open follow-up: full O-1 fingerprint/vehicle continuity is unavailable from
current data; do not infer match from kind1. P4 must keep dynamic binding
Unbound unless its independent evidence and stationary owner confirmation
requirements are met, respect dynamic-activation capability bit0-only, and
verify #142/RF timing separately. No owner decision is needed to review this
read-only P3 addition; additional acquisition/TX/activation is outside it.
