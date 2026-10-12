# PR15 integration on updated main (2026-10-12)

Base: main `3d53e9f`, including PR2, PR5, PR10, PR13 and the accepted PR14 fixes.
PR15 was rebased onto this main; both the fixed CAN-owner scratch storage and
correct NimBLE core pinning are retained. The duplicate ADR number was resolved
by preserving CAN stack D018 and renumbering the multi-PID pilot to D019.

## Scope of integration

Grouped `01 0C 0D` CAN requests are enabled only in the new explicit
`m5dial_dual_ble_multipid_bench` profile. Existing normal, dual 20-byte,
MTU247 and mode-switch dry-run builds continue individual requests. The
physical CAN and vehicle acceptance gates remain pending. This merges the
implementation and host evidence, not a claim of vehicle qualification.

## Host evidence

`tests/test_dual_ble_adaptive.cpp` runs the production dual-session scheduler,
ELM parser, ISO-TP transaction parser, response formatter and targeted BLE
notification code against a host-only simulated ECU backend. It checks:

- both clients get the existing RPM=1250/speed=80 per-item reply contract;
- successful capability probing remains enabled for subsequent batches;
- partial, negative, silent, split-ECU and malformed First Frame responses
  cause exactly one grouped probe per boot, then individual reads;
- a second client staged before a failed probe uses the individual path;
- a different responder after a successful probe disables grouping;
- lease, bus and driver failures stop the remaining grouped owner's batch;
- disconnecting the probing peer gives no output to the disconnected peer
  and the remaining peer uses individual reads;
- DEMO causes no backend diagnostic requests;
- compiling without the new opt-in flag preserves individual CAN requests.

This backend is not TWAI and does not prove electrical CAN transmission,
termination, real ECU timing, hardware BUS-OFF or actual throughput. Existing
pacer, read-only policy, stale-frame, ISO-TP and protocol regression tests remain
part of `tests/run_all.sh`. Five PlatformIO profiles and compiler stack frame
budgets are included in CI.

## Remaining acceptance

Run the physical simulated-ECU procedure in `adaptive_multipid_pilot.md` before
vehicle use. Long-duration testing is deliberately deferred at the user's
request. Neither CAN compatibility nor a performance improvement is claimed.
