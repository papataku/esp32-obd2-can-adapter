# P2: adapter legacy-rp8 built-in profile data

Design: [24fbd7d8308729ecfa69cae3cdc3fd402af1cff0](https://github.com/papataku/tacotaco-meter-ios/blob/24fbd7d8308729ecfa69cae3cdc3fd402af1cff0/docs/protocol/vehicle-profile-ecu-id.md), §1/2/7 P2/8, O-5/O-6.
P1: [TacoTacoMeter cc8caca63db99e51dde7a5518c9a939d29a5ec6a](https://github.com/papataku/TacoTacoMeter/blob/cc8caca63db99e51dde7a5518c9a939d29a5ec6a/src/vehicle/vehicle_profile.h) (PR #143).
Adapter fixed baseline: `de635e9f628d46e26050019c15cf337482789276`.

`include/signal_v2_vehicle_profile.h` contains only the reviewed `legacy-rp8`
seed: schema **1**, revision **1**, Extended29, responder `18DAF101`, physical
request `18DA01F1`, the same five PIDs in P1 order `0D/0C/05/5B/9A`, existing
probe/group payloads, pacing, reservation sizes and budget limits. The evidence
reference is the fixed baseline discovery header and its SHA256. It is audit
provenance, not a signature, owner authentication, vehicle identity or TX grant.
No persistent store, binding, selection, import, revocation or NVS migration is
introduced. O-6 persistence/trust work remains for later slices.

The existing `M5CAN_SIGNAL_V2_RP8_PROFILE` build opt-in reads the candidate from
that data. Without it the candidate count remains zero. Built-in lookup rejects
null, empty and unknown IDs and returns zero candidates. There is no runtime
profile selector, automatic selection, new profile, fallback ID or new TX path.
The pinned route and independent lower-level request validation remain intact.
The data is a C++11 aggregate; it is never serialized as a wire PDU.

CanOwner reads the same probe/group bytes, 120ms deadline, 200ms probe/5Hz and
100ms 10Hz-lab floor, and 4/12/4/16 frame reservations/headroom. CanSamples reads
the same PID order. CanBudget reads 200bits/frame and the same rolling
5000bits/100ms and 25000bits/1s caps, for both admission layers and observed RX.
Reservation lifetime and all admission comparisons are unchanged. The session
3s KeepAlive lease, owner transaction 550ms lease grants, discovery 3s window,
S1 observation window and authorization epochs are unchanged in their existing
code. Wire codecs, session formats and the lower CAN driver are frozen.

**Owner-live and staged lab remain separate.** The named owner-live image alone
keeps its secured-session 2s settle auto-ARM, power-cycle fault latch and tap
STOP. Staged images retain STAGE_GATED and their independent S1 authorization.
GATT, UI, advertising/authentication/registration and NUS files are unchanged.

## Reproducible verification (no devices)

- `./tests/run_all.sh`: existing tests, plus `bash tests/run_p2_profile.sh`.
  Use Python >=3.10 as required by existing native parser tests.
- `bash tests/run_p2_profile.sh`: C++11 schema/route/PID/limit checks with and
  without the RP8 opt-in; unknown/empty lookup has zero candidates. Existing
  review-owner tests exercise actual no-profile CanOwner with zero submissions.
- `python3 tools/test_p2_profile_equivalence.py --output <directory>`: archives
  the immutable baseline above (fetches that exact object in shallow clones),
  builds baseline and current sources separately, compares byte-identical
  transcripts. It never substitutes current sources for the baseline.
- Host mode preserves every assertion of the existing owner-live fixture and
  records requests and owner budget usage. Serial mode compiles production
  CanOwner + CanMonitor + diagnostic transaction/ISO-TP + the real serial TWAI
  shim. A test-only wrapper schedules synthetic ECU RX at the shim boundary;
  no serial port is opened. It records real request/FC frame bytes, destination,
  submission times, both admission budgets, observed budgets, epoch and STOP.
  5Hz and 10Hz demand cases include the existing essential shedding behavior.
- The same comparison always checks an exact 5000bit budget boundary.
  `python3 tools/test_p2_profile_equivalence.py --mutation-proof` changes the
  limit to 4999 only in a temporary copy and **must exit 1**. Baseline admits
  `1,5000`, mutant returns `0,0`; the working source is untouched.
- `python3 tools/test_p2_profile_scope.py`: fixed-base file allowlist and
  byte-level freeze of wire/PDU, CAN driver, lease/session, GATT/UI/auth/BLE/NUS,
  config and existing assertions, plus owner authorization blocks.
- Existing `test_signal_v2_auto_arm.py`, `test_signal_v2_b01_policy.py`,
  `test_signal_v2_disconnect_owner.py`, revoke-race and HIL regressions retain
  2s settle, link-loss epoch, fault/no-reARM, tap STOP/no-TX and stale RX checks.

This is host execution of serial-HIL code, not a board-connected serial run.
Device, physical serial, RF/LCD/touch, bench CAN and car acceptance: **NOT RUN**.
No device, flash/upload, serial port or physical CAN TX is used.
