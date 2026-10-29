# Safety and Vehicle-Connection Rules

This project connects directly to an in-vehicle CAN network. Development must assume that malformed or excessive diagnostic traffic can disturb ECUs or vehicle communication.

The project therefore uses a staged, fail-safe approach.

## 1. Power and wiring

Normal vehicle configuration:

```text
ACC/IG-switched USB 5V
        |
      M5Dial
        |
        +-- 5V/GND --> U179
        +-- GPIO13 --> U179 CAN_TX
        +-- GPIO15 <-- U179 CAN_RX

Vehicle OBD:
Pin 6  --> CAN-H
Pin 14 --> CAN-L
Pin 4/5 --> GND
Pin 16 --> not connected
```

Do not normally power the project from OBD Pin 16 because it is battery/always-on power on standard OBD-II wiring.

The intended behavior is:

```text
vehicle/ACC off
 -> USB 5V off
 -> M5Dial off
 -> U179 off
```

## 2. U179 termination

U179 has an onboard 120-ohm CAN-H/CAN-L termination resistor.

A typical already-terminated CAN bus has two 120-ohm terminators, appearing as about 60 ohms across CAN-H/CAN-L when safely measured with power removed.

Adding another 120-ohm termination in parallel can move the effective resistance toward about 40 ohms.

Therefore:

1. Do not assume the RP8 diagnostic connector topology.
2. Measure the adapter standalone.
3. Measure the vehicle bus only with the vehicle powered down and only when it is safe to use resistance measurement.
4. Confirm whether the U179 termination must be disabled.
5. For permanent/normal vehicle attachment, do not knowingly leave an unnecessary third terminator on the bus.

Removing/disabling hardware termination is a hardware modification and should be verified electrically afterward.

## 3. Non-isolated interface

U179 is non-isolated.

When M5Dial is connected to the vehicle and USB is connected to a Mac, the vehicle ground and USB/Mac ground are electrically related through the setup.

During early bench/vehicle analysis:

- keep the wiring simple
- avoid unnecessary additional grounded equipment
- verify ground paths
- prefer a controlled development setup

An isolated CAN interface may be considered later if needed, but the current baseline remains U179.

## 4. Boot behavior

Boot must lead to Listen-Only.

```text
Power on
 -> initialize state
 -> TWAI Listen-Only
 -> receive/observe
 -> wait for explicit host action
```

No saved setting may cause automatic diagnostic TX immediately after boot.

## 5. TX Lease

Any active diagnostic transmission capability requires a time-limited lease.

The lease must be checked at the real transmit boundary, not merely when the command was accepted.

Example:

```text
Host requests lease
 -> firmware grants <= configured maximum
 -> request is validated
 -> TX boundary checks lease deadline
 -> transmit
 -> lease expires
 -> TX denied immediately
 -> driver returns/remains in safe state
```

The host may renew while an intentional diagnostic session is active.

If renewals stop, TX stops.

## 6. Allowed services

Development starts with read-only diagnostic operations.

Do not expose unrestricted arbitrary payload transmission as an easy public command.

Early phases should reject write/control services and malformed request lengths before they reach CAN.

UDS features such as these are out of initial scope:

- SecurityAccess
- WriteDataByIdentifier
- RoutineControl
- ECUReset
- programming/download services
- actuator/control services

ReadDataByIdentifier may be added in a later controlled phase.

## 7. ISO-TP Flow Control ownership

A Flow Control frame is itself a CAN transmission.

Do not send FC merely because a First Frame arrived on an expected-looking response ID.

Before sending FC, confirm that the response belongs to a currently active request owned by this adapter, using request/response context such as:

- expected response ID
- service response
- PID/DID context where available
- query start timestamp/window

This avoids interfering with another tester communicating on the same bus.

## 8. Time and queue correctness

Known failure modes to guard against:

- stale RX frames from before a query being accepted as the new response
- response already queued but query timeout processed first
- host flooding serial input and delaying lease expiry handling
- driver mode switch racing the RX task

Tests must explicitly cover these.

Use frame receive timestamps when deciding whether a response belongs to a query; do not rely only on the time the application eventually dequeues it.

## 9. Bus errors and BUS-OFF

BUS-OFF is not a reason to automatically resume diagnostic work.

Preferred policy:

```text
BUS-OFF/error
 -> revoke TX lease
 -> stop diagnostic TX
 -> record error/event
 -> recover CAN controller only under controlled logic
 -> remain TX-disabled
 -> require explicit new authorization before diagnostic TX
```

## 10. Rate limiting

Diagnostic throughput must be increased gradually.

Start conservatively, measure:

- response latency
- timeout rate
- CAN error counters
- BUS-OFF/error events
- negative responses
- bus utilization where available

Do not optimize for maximum requests/sec at the expense of vehicle stability.

## 11. Vehicle test progression

Required progression:

```text
host unit tests
 -> real PlatformIO build
 -> no-CAN / loop sanity
 -> bench CAN
 -> vehicle Listen-Only
 -> vehicle known read-only OBD query
 -> controlled ISO-TP
 -> controlled UDS read
 -> performance tuning
```

A later phase must not be used to compensate for an unverified earlier phase.

## Signal V2 build and rollback boundary

The default `m5dial` remains the existing request-driven, TX-capable ELM327
adapter by explicit owner decision. It is **NOT a CAN-disabled fallback** and
does not use the Signal V2 USB ARM lifecycle. V2-1
`m5dial_signal_v2_can_disabled_lab` compiles out TWAI initialization and TX;
CI checks its linked ELF for `twai_transmit`, `twai_driver_install`, `twai_start`.
Only V2-2 environments enable the Signal V2 TX path. Legacy TX-capable
profiles are separate and are not Signal V2 fallback images. Rollback must
use the owner's verified original backup image, never a rebuilt default.

See the [first-trial runbook](qa/signal-v2-2-29bit-vehicle-runbook.md) for
per-board backup/readback/restore, mandatory independent isolated-bench load
measurement, passive versus armed phases and STOP/expiry/no-TX verification.
Current V2-2 functional discovery is deliberately refused: the maximum
induced responder traffic cannot fit the admitted budget. This closes the
unbounded broadcast path but does not qualify active vehicle acquisition.

Dedicated ambient-only candidate: `m5dial_signal_v2_can_29bit_passive` removes
all controller TX GPIO routes and fixes U179 TXD HIGH. It has no ARM/lease/BLE
application. Use only its [passive acceptance runbook](qa/m5dial-passive-runbook.md)
and exact [routing evidence](qa/m5dial-passive-design-review.md); a build alone
is not a vehicle GO. Existing active profiles still need their own gates.
