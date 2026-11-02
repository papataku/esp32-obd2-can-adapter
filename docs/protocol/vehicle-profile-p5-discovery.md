# P5: CAN-less 11-bit functional discovery lab

Design input: `papataku/tacotaco-meter-ios`,
`docs/protocol/vehicle-profile-ecu-id.md` at
`24fbd7d8308729ecfa69cae3cdc3fd402af1cff0`, §1/2/7/8/9 O-3.

Vehicle TX is NOT approved. The lab environment must not be flashed to a car
device without separate written owner approval naming the vehicle and conditions.
This slice never opens a physical CAN device: its TWAI API resolves to the
existing USB serial HIL shim. owner_live and existing environments retain their
pre-P5 code, flags, wire, BLE/auth/registration and NUS behavior.

Only `m5dial_signal_v2_can_11bit_discovery_lab` compiles discovery, with
`M5CAN_SIGNAL_V2_11BIT_DISCOVERY_LAB=1`, `STAGE_GATED=1`, and `SERIAL_HIL=1`.
It has a headless USB entrypoint and no BLE, UI, profile store or auto-ARM.
The compile guard rejects discovery together with RP8, owner_live or auto-ARM.

The exact single-frame request is standard ID **0x7DF**, DLC **8**, data
**`02 01 00 00 00 00 00 00`**, single-shot (no retry). It is Service 01 PID 00.
No 29-bit ID conversion, physical request, fallback, ISO-TP FC, or second query
is authorized. Compile-time constants/assertions pin TX1, all-RX16, responders8,
FC0, 3,400 bits, receive120ms, observe1000ms, total3000ms.

A dedicated `P5 ARM` admits exactly one query only from stopped/non-LIVE state.
The same CanOwner, lower CanMonitor, rolling budgets, epoch revocation and STOP
TX mutex used by the existing implementation enforce admission. Both owner
and monitor reserve `(1+16)*200 = 3400` bits before TX; reservations are held
through the 1s observation interval and their rolling expiration, including
across a new ARM. The lower gate also checks measured traffic. Neither journal
is reset by ARM. Limits remain 5000 bits/100ms and 25000 bits/1s.

Each candidate must be a standard, non-RTR, non-self DLC8 SF from 0x7E8..0x7EF:
`06 41 00 A B C D pad`. Its bitmap is `A<<24 | B<<16 | C<<8 | D`.
Responses must arrive within 120ms of TX. All RX continues to be monitored for
at least 1s after TX, even after candidates arrive; late traffic is a fault.
The total deadline is 3s from ARM, including queue/admission delay. Any FF/CF,
FC attempt, duplicate, unexpected ID/width/format, ninth response, seventeenth
RX, overrun/drop/bus error, timeout, TX failure, epoch/profile conflict, or
conflicting second TX stops the run and suppresses its candidates. Failed runs
latch the existing monitor fault; recovery requires a new process/boot, never
automatic re-ARM. Successful STOP allows a new explicit ARM with a newer epoch,
subject to retained rolling budgets and rejection of old HIL generations.

There are only eight admissible distinct 11-bit IDs. Thus a ninth on-wire
responder is necessarily also duplicate or out of range. Tests separately
exercise the count guard at eight and an eight-plus-ninth serial transcript;
they do not invent a ninth admissible ID or truncate a 29-bit address.
The seventeenth-RX counter guard is independently tested because malformed or
duplicate earlier RX must already stop a real run.

On success only, output is sorted by full responder ID:
`#P5,Learning=1,LIVE=0,epoch=<n>,id=7E8,bitmap=12345678`.
Candidates and raw capture are bounded RAM only; HIL evidence goes to USB.
No automatic profile, Bind, persistence, MAC/VIN, PID sample or LIVE acquisition
is created. Driver generation and authorization epoch are separate counters.

## USB commands (lab only)

- `P5 ARM`: explicit one-query permission; duplicate ARM cannot renew it.
- `P5 STOP`: revoke permission immediately.
- `P5 PROFILE CHANGE`: revoke permission and epoch before any further result.
- Existing `HIL STATUS`, `HIL RX <driver-generation> <hex-id> <0|1 ext> <hex-data>`,
  `HIL TXFAIL 1`, `HIL RXMISS 1`, `HIL BUSERR 1`, `HIL OVERFLOW`, `HIL BUSOFF`
  operate solely on the serial TWAI shim. Use the current install generation
  from `#HIL_EVENT` after the normal-mode switch, not the authorization epoch.

There is no discovery wire opcode. Generic `ARM`/`SIGV2 CAN S1` do not authorize
this entrypoint. Serial input is bounded to 128 bytes per loop; owner task
processing remains independent. This PR's tests call the actual USB parser and
TWAI shim in a host process; no board or serial port is used.

## Reproducible checks

Run with Python 3.11: `tests/run_all.sh`; focused checks:
`python3.11 tests/run_p5_discovery.py`, `python3.11 tools/test_p5_scope.py`,
`python3.11 tools/test_p5_mutations.py`. Mutations live only in temporary copies
and must fail assertions for ninth response, FC, one query and env isolation.
The P3 scope test retains its immutable-base assertions and removes only
identified P5-guarded additions before comparing legacy production files.
The P5 scope test pins the P3 merge and compares preprocessed tokens for every
existing env, including inherited flags, and rejects owner_live+discovery.
Fixed base commits are fetched on demand in shallow checkouts.

Build all envs with PlatformIO; synthetic test-only identity/fresh-flash build
acknowledgement are required by existing guards for secure lab envs. They are
compile inputs, never deployment approval. Run `tools/check_p5_elf.py --all
--nm <xtensa-esp32s3-elf-nm>` after builds. Compare owner_live firmware to the
fixed P3 base built with the identical source stamp; record raw image SHA256
and byte equality separately. GCC14 `-Wall -Wextra -Werror` in `python:3.12`
also runs the focused runtime, preprocessor and mutation checks.

Physical serial HIL, bench CAN, vehicle TX, flashing and vehicle response-count
validation are **NOT RUN** and not approved by host/HIL success. This code-only
slice precedes an independent safety review and owner vehicle approval.
