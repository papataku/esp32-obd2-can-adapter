# V2-2 receiver safety contract and owner provisioning

This is a software acceptance contract, not approval to flash or use a vehicle.
The receiver lifecycle sections were supplied by the companion Ring review;
adapter tests verify the CAN wire portion, not the receiver UI or hardware.
V1 runtime remains compiled out of all three Signal V2 environments.

## CAN wire extension (paired adapter PR #63)

Legacy DEMO fixtures retain record type 1 / Snapshot prefix 16. A CAN receiver
requires record type 2 (same 44-byte fixed header): offset 34 is now unsigned
`AgeAtEncodeUs`, the age of `ObservedAtUs` at PDU encoding, in microseconds.
All records in a PDU share nonzero SourceGeneration / ModeGeneration. Both
advance across authorization STOP/ARM; a receiver accepts a generation change
only in a verified Snapshot, rejects generation rollback, and resets acquisition
history only for that change or a new boot/session/profile.

CAN Snapshot has byte 13 = 2, bytes 14..15 = 0, an echoed 32-bit Snapshot
Control RequestId at 16..19, and records beginning at offset 20. ResumeSequence,
flags, revision and count retain offsets 0, 4, 8 and 12. The echo binds the
Snapshot to the current request; an old queued transfer cannot borrow a newer
request's timing. Retries keep the original request time and ID.

For the first valid record, remote encode time E = ObservedAtUs + AgeAtEncodeUs.
The initiating request's local start Q is a lower bound on local encode time.
Thus at local receipt N, an acquisition A has conservative age
`N - Q - (A - E)/1000`, rounded conservatively with 2ms margin plus 1000ppm
relative clock drift allowance. The paired monotonic clocks must satisfy that
bound; this is not an assumption of synchronized wall clocks. It covers delay
before FIRST, between fragments, and in both notification/controller and worker
queues. Later DATA uses the same mapping. Mapping expires at 10s; receiver
requests a new correlated Snapshot every 5s. CAN Fresh ends after the requested
400ms freshness; Invalid begins beyond the admitted 600ms deadline. Excessive
RTT is fail-closed, even if it makes a genuinely recent sample unusable.

Same-epoch requireSnapshot hides display values but retains ECU route pin and
per-subscription sequence, raw digest, acquisition timestamp and age. Missing or
omitted entries also retain anti-replay history. Duplicate sample age cannot
decrease; a duplicate timestamp with a new sequence is rejected. Heartbeats do
not renew any sample. An empty Snapshot cannot establish a usable clock bound.

## Explicit owner states on the BLE page

Double-tap SCAN within 10s only when physically parked. The prompt explicitly
identifies first-use offline setup and target change. The worker independently
checks the state after the UI command, before stopping the existing link:

- **Offline first use:** no saved target, no connected link, no ready data,
  no subscription. This is an explicit owner provisioning state, never evidence
  that unknown speed is parked. A Ring with a saved target cannot enter it.
- **Stationary target change:** connected, link ready and subscribed, with
  real CAN Fresh speed in 0..0.1 km/h. DEMO, unknown, delayed, missing and moving
  speed all refuse entry. Owner action leaves driving use, disconnects the old
  peer, resets the old session, and starts a bounded 30s provisioning window.
- **Selection window:** SCAN and choose both require the provisioning state,
  no connected link, no ready data and no subscription. The former contradiction
  between requiring a live view and forbidding a live link is removed by the
  explicit state transition. Expiry returns to the saved target.
- **Candidate verification:** explicit ephemeral-token choice ends provisioning
  and starts a 90s deadline. Only encrypted GATT, accepted HELLO/owner approval,
  accepted profile and validated initial Snapshot permit NVS replacement.
  Authentication, connection, timeout or storage failure clears the candidate
  and restores the previous target. Bonds are never erased as recovery.
- **Reboot:** load only the committed NVS target; unverified candidates are never
  stored. All UI candidates expose name/RSSI/token, no BLE address.

A disconnected Ring with a saved target and unknown speed stays locked. Park
and reconnect to obtain Fresh zero before changing target. No driving grace
period or unknown-speed override exists. Stationary confirmation is an owner
responsibility during the short disconnected provisioning window.

## Verification boundary

`test/test_signal_v2_worker.cpp` includes the production client translation unit;
only Arduino/NimBLE/FreeRTOS/NVS boundaries are faked. It exercises UI handoff
APIs, real command queue processing, requireSnapshot, wire decoding and
fragmentation, HELLO/profile/Snapshot ACK handling, NVS replacement and rollback.
The transition/SCAN/choose composition covers all eight connected/ready/
subscribed combinations with saved/first-use, valid/unknown and zero/moving
speed. `tools/test_signal_v2_ring_policy.py` executes this composition in addition
to preserving the UI/source invariant. Physical radio, touch and vehicle safety
acceptance remain separate owner gates.
