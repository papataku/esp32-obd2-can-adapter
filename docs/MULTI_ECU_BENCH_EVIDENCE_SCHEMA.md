# Offline CAN/BLE bench evidence contract (JSONL schema 1)

**Status: simulator / QA tooling, not hardware qualification.**

This validator deliberately has **no CAN transmission API**, no Bluetooth access,
no GPIO handling and no path to enable vehicle diagnostics. It reads a local
JSONL file, checks basic safety/provenance invariants, and exits with code 1 on
any mismatch. Passing generated/synthetic input proves *only that the
validator recognizes its own fixtures*, never that physical hardware was
tested.

## Purpose

The multi-ECU program has three distinct gates:

1. Unit tests: mock UDS22 requests to ECU01/02/06/0E/EF, ISO-TP and dual BLE
   connection generations; physical TWAI TX remains disabled.
2. **Offline evidence checker (this PR)**: validate a recorded CAN+BLE
   observation against strict invariants. Hardware evidence is required
   separately; the current tests use synthetic fixtures.
3. Isolated CAN simulator / real BLE devices: only after independent
   qualified review and a separately designed bench-only TX profile.
   It is prohibited to interpret schema acceptance as approval to drive a car.

## Required trace fields

Each nonblank line is one JSON object. The first is metadata:

```json
{"type":"meta","schema":1,"profile":"isolated_can_bench","evidence":"synthetic","firmware_sha":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}
```

A `passive_tx_off` profile requires at least one `request_denied` event
with `reason="tx_off"`; **any** `can_tx` event must fail.

An `isolated_can_bench` profile requires at least one completed UDS22
physical-read request, at least one CAN transmit record and at least one BLE
delivery. Accepted CAN request IDs are exactly:
`18DA01F1`, `18DA02F1`, `18DA06F1`, `18DA0EF1`, `18DAEFF1`.
For each accepted `18DAxxF1`, the only corresponding response is
`18DAF1xx`. Only `22` service with a four-character uppercase DID may be
started. One request may be outstanding at a time.

The event grammar is described in the docstring of
`tools/validate_multi_ecu_bench_trace.py`, and examples are exercised in
`tests/test_multi_ecu_bench_trace.py`. The validator checks:

- Monotonic event timestamp and strictly increasing BLE connection
  generation on handle reuse.
- No simultaneous physical requests or unauthorized header/service.
- No Flow Control without a matching First Frame, no FC to a different ECU;
  no CF accepted without FF + FC and monotonically correct CF sequence.
- No successful request without matched positive UDS data.
- A joined BLE peer must have the **same ECU, service and DID**.
- No delivery to a disconnected/old generation or an unjoined BLE peer.
- No transmit after a logged CAN fault lock.
- No silent classification of test-generated samples as hardware evidence.

A trace with `"evidence":"hardware"` requires `capture_sha256` (64 hex
characters), `observer`, `interface`, and `firmware_sha`. Each `can_tx`
or `can_rx` event **must also contain actual observed CAN payload bytes**:

```json
{"type":"event","t_us":12,"kind":"can_tx","id":"r02","frame":"request","can_id":"18DA02F1","data_hex":"0322201200000000","dlc":8,"extended":true,"rtr":false}
```

For hardware evidence, the validator derives PCI, service, DID, and CF
sequence from the raw bytes rather than trusting provided string labels;
it checks exact eight-byte UDS22 request and FC framing, DLC, and 29-bit
data-frame flags. The CF sequence may wrap from 15 to 0. One BLE
connection generation may receive at most one output per request.
When hardware bytes are provided, the verifier also checks the first-frame
declared length against the firmware's **96-byte** diagnostic buffer,
reconstructs the received payload length (6 initial bytes + up to 7 per
Consecutive Frame), rejects a premature or false `complete` label, and
rejects any matching CF arriving **100,000 microseconds or more after** the
previous matching FF/CF. This mirrors the default `IsoTpAssembler` deadline.
If firmware changes its timeout/buffer policy, the schema and tests must
be reviewed and versioned together.

The source/provenance fields are still **declared provenance only**. The
checker does not authenticate who took a capture or independently prove
that a claimed SHA matches external hardware bytes. QA must cross-check
the independent CAN analyzer's original recording and the entered hash,
plus firmware SHA, BLE log, timestamp alignment, and any drop counters.
*Never upload sensitive real vehicle raw captures to public GitHub PRs.*
Store evidence in the approved private artifact store.

## Commands

```bash
PYTHONPATH=tools python3 tests/test_multi_ecu_bench_trace.py
python3 tools/validate_multi_ecu_bench_trace.py path/to/capture.jsonl
```

`PASS SYNTHETIC (NOT HARDWARE EVIDENCE)` is the expected result for unit
fixtures. A real trace would report `PASS HARDWARE TRACE STRUCTURE` only
after its schema/invariants pass; this still is **not** a release approval.

## Open items before independent hardware test

- Design an export adapter that creates this JSONL from the actual
  instrumented M5CAN and a second independent CAN analyzer; pin each
  firmware/config SHA and a shared monotonic clock/correlation marker.
- Capture raw frames and BLE generation identifiers from the hardware test;
  validate clock drift/ordering and correlate CAN TX with external analyzer.
- Measure loss/overflow/queue drop counters, p50/p95/p99 response duration,
  CPU stack high-water, lease expiration, bus-off and reboot recovery.
- Document explicit physical isolation, termination, emergency CAN power
  removal and tester-controlled ECU simulator addresses.
- Independent human CAN, vehicle safety, BLE/RTOS, iPad and QA sign-offs.
- Only after all above and a separate reviewed TX capability contract,
  consider stationary, read-only vehicle testing. No unknown DID scans in
  motion; freshly confirmed P + stable zero-speed gating remains required.
