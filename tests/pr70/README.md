PR70 stale owner authorization regression
=======================================

`python3.11 tests/test_signal_v2_revoke_race.py` compiles the unchanged production
CanOwner and CanMonitor plus the actual GATT grant, disconnect and tap STOP
bodies (extracted as in the existing auto ARM fixture). Only host IO is replaced.
The queue stub copies the actual request, rather than reconstructing it.

Each process runs one interleaving. A positive run enumerates every critical
exit, mutex take/give, queue-copy boundary, driver status and RX boundary from
tick entry through the first TX admission. Then both production disconnect
and tap STOP are inserted at each point: this covers the final health check,
before/after acquireLease, before/after enqueue, and before TX admission.
A callback arriving inside the task mutex completes immediately after release;
a TX already holding the mutex precedes revocation and is counted separately.
Every run asserts zero driver calls after completed revocation, no old-epoch
lease reacquisition, and no old request/FC borrowing a new valid epoch's lease.
Positive controls require a real driver submission for the current epoch.
Monitor builds cover owner_live, shared unstaged, staged 5Hz and staged 10Hz.
The existing HIL regressions and ELF check separately prove HIL isolation.

For before/after evidence, set `PR70_SOURCE_ROOT` to an archive of the old SHA.
The harness uses the old API when needed, without changing production sources.
The same assertions fail on 8e23e42 (56/56 owner interleavings); this is an
expected failing regression run, not a passing acceptance result.
