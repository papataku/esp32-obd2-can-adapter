# Phase A: TX-disabled multi-ECU physical UDS22 routing test

## Why this change

The two RP8 iPad sessions recorded on 2026-10-18 returned `?\r>` for 1,120
and 824 attempted UDS22 reads of ECU 02/06/0E/EF. M5CAN's normal policy
allows physically addressed UDS22 only to ECU 01, even though the car has
been observed replying from other ECU IDs to diagnostic requests. **A
responder ID seen under functional Mode01 does not demonstrate UDS DID
support at that physical address.** We must not interpret these adapter
rejections as negative responses from the vehicle.

## Scope: test-only

`m5dial_multi_ecu_policy_dry_bench` is a dedicated opt-in build which
inherits `M5CAN_CAN_MODE_DRY_RUN`. The TWAI physical transmit API is
compiled out and all attempts to transmit return failure. The experimental
`M5CAN_MULTI_ECU_POLICY_DRY_BENCH` flag **must cause compilation to fail**
when the TX-disabled flag is absent.

Only in that opt-in build, physical routing accepts exactly the following
read-only UDS `22 DID` request IDs:

| ECU source byte | Request CAN ID | Required response CAN ID |
|---|---|---|
| 01 | 18DA01F1 | 18DAF101 |
| 02 | 18DA02F1 | 18DAF102 |
| 06 | 18DA06F1 | 18DAF106 |
| 0E | 18DA0EF1 | 18DAF10E |
| EF | 18DAEFF1 | 18DAF1EF |

All other physical addresses remain denied. Normal production/dual BLE
firmware still only allows physical ECU01.

The host test sends simulated same-DID and wrong-DID First Frames and
Consecutive Frames from both the correct responder and an unrelated ECU.
Only a matching positive response from the requested ECU can generate
`NeedFlowControl`; the FC arbitration ID must equal that physical
request ID. Wrong ECU and wrong DID responses must be ignored.

This PR does **not** add a new BLE API, extend `ATM5B01`, or announce an
extra `ATM5CAP` capability. iPad must continue rejecting unsupported
ECU sources when talking to M5CAN PROTO 1.1. It does **not** turn on any
additional real vehicle CAN transmission.

## Gates before expanding real-vehicle functionality

1. Host policy/ISO-TP tests pass with source/payload permutations, wrong
   DID, wrong service, invalid header, and strict FC target checks.
2. Compile-time negative test demonstrates expansion is impossible
   without `M5CAN_CAN_MODE_DRY_RUN`.
3. Dedicated PlatformIO dry-run build succeeds; existing `m5dial` and
   dual-BLE builds/tests remain unchanged.
4. Design a separately versioned, explicitly negotiated ECU allowlist
   and a bounded diagnostic request path without altering legacy 1.1.
5. Independently review CAN owner, TWAI mode transitions, lease expiration,
   per-DID pacing, NRC 0x78, malformed ISO-TP, concurrent BLE and bus-off.
6. Controlled CAN simulator bench with actual CAN transmissions only
   after authorization (different firmware/PR). Parked vehicle read-only
   acceptance on a selected ECU after the simulator gates pass.
7. No unknown DID scan while driving; iPad has manual P confirmation and
   fresh/stable 0 km/h enforcement.

## Test command

```bash
./tests/compile_firmware_host.sh
pio run -e m5dial_multi_ecu_policy_dry_bench
```

Never flash the dry-run build to a vehicle expecting diagnostic acquisition:
it deliberately cannot send CAN.
