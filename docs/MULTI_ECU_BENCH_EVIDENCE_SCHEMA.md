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


## Independent CAN analyzer reconciliation (second, stricter gate)

The event trace must not be regarded as independent evidence of real CAN
transmission: an internal trace can report a request, FC, or success that a
separate physical CAN analyzer never observed. Before any bench signoff,
record an **independent analyzer export** as canonical JSONL:

```json
{"type":"capture_meta","schema":1,"lost_frames":0}
{"t_us":12000,"can_id":"18DA02F1","data_hex":"0322201200000000","dlc":8,"extended":true,"rtr":false}
```

Every row is a Classical CAN frame observed by the external analyzer, not an
application-generated log entry. Retain 11-bit data frames, 29-bit data
frames and remote (RTR) frames in their observed order; these are allowed as
background traffic, but **diagnostic** matches must still be 29-bit data
frames with exact payloads. Store the exact SHA-256 of that JSONL file
as `capture_sha256` in the event trace metadata and preserve both files.
`lost_frames=0` must be independently checked against the analyzer's
interface statistics; a self-declared counter is not trustworthy by itself.

Run:

```bash
python3 tools/reconcile_multi_ecu_can_capture.py \
  capture_events.jsonl independent_can.jsonl \
  --clock-offset-us 0 --max-skew-us 5000
```

The explicit clock offset means **independent CAN clock minus event-log clock**.
Determine it using a shared hardware trigger or timestamp synchronization
measurement. A loose tolerance is not proof of synchronized clocks.
The tool verifies the canonical file's actual SHA-256, exact ordered
CAN ID + payload + DLC + extended/RTR flags within the measured timestamp
tolerance, and rejects **any** independently observed physical tester frame
(`18DAxxF1`, `18DB33F1`, `18DBEFF1`) missing from the application
TX events. In the `TX=OFF` profile, any such physical tester TX fails.
Unrelated bus background receive frames are permitted, but a real isolated
bench must have independently documented sole control of the tester IDs.

This is a **necessary but insufficient** acceptance gate: an operator could
construct two matching fake files, or a capture adapter could misattribute
direction or timestamps. Independent human review must verify analyzer
configuration, isolation, physical connection, SHA values and raw capture
continuity. All CI fixtures are **SYNTHETIC ONLY** and never serve as real
hardware qualification.

## Optional offline importer for Linux can-utils logs

To make the canonical JSONL reproducible, a strict importer can read an
already-captured **Classical CAN** `candump -L` text file, for example:

```text
(1609770956.314100) can0 18DA02F1#0322201200000000
(1609770956.314110) can0 18DAF102#100A622012010203
(1609770956.314120) can0 123#R8
```

After independent capture and review of the interface's actual packet-loss
counter, use:

```bash
python3 tools/import_candump_independent_can.py \
  original_candump.log independent_can.jsonl \
  --interface can0 --lost-frames 0
sha256sum original_candump.log independent_can.jsonl
python3 tools/reconcile_multi_ecu_can_capture.py \
  capture_events.jsonl independent_can.jsonl \
  --clock-offset-us <measured_offset_us> --max-skew-us 5000
```

The converter writes new files only, never overwrites evidence, and records
the SHA-256 of the original `candump` text, selected interface and measured
loss count. Converted CAN timestamps are exact integer microseconds relative
to **the first CAN frame** (no floating-point roundoff); the clock offset
must account for this origin when cross-referencing the M5CAN/ BLE event log.

The importer **does not run candump or open SocketCAN**; an independent
operator must obtain the actual capture separately. It deliberately rejects
CAN FD `##` syntax, non-`-L` formats, missing timestamps, mixed
interfaces, invalid RTR/data, malformed lines or regressed timestamps,
rather than silently filtering potentially safety-relevant frames.
Only Classic CAN is supported in this evidence schema. A zero loss counter
must be based on independent analyzer status, not guessed.

As elsewhere in this document, test logs created in CI are synthetic,
not proof that a hardware capture exists.

## Import actual M5CAN Native USB capture (device-side corroboration)

M5CAN already implements a binary Native USB protocol with COBS framing,
CRC32, packet sequence numbers, `CAN_FRAME` timestamps, `HELLO`
device/firmware metadata and `STATS` drop/overrun counters.
A **previously captured** Native byte stream may be converted offline:

```bash
python3 tools/import_m5can_native_capture.py \
  original_m5can_native.bin m5can_device_rx.jsonl
sha256sum original_m5can_native.bin m5can_device_rx.jsonl
```

The converter enforces packet framing, CRC, sequence continuity (including
65535 → 0), timestamp ordering, Classical CAN DLC and ID ranges, HELLO
identity, nonzero frame count, and STATS completeness. Any reported dropped
frames, overruns, driver errors or invalid data cause failure. It retains
the input file's SHA-256, CAN IDs, bytes, device times, and `self_reception`
flags. It refuses to replace an existing evidence file.

**Critical provenance distinction:** The result deliberately has a
`"type":"device_capture_meta"` header and
`"independent_analyzer":false`; it is **not** compatible with the
`capture_meta` input required by
`reconcile_multi_ecu_can_capture.py`. That is intentional: a
device-originated RX capture cannot serve as independent proof of TX
permission, CAN frame emission, absence of unexpected FC, or actual BLE
peer delivery. The `HELLO.tx_enabled` field is only a declaration from
firmware, and `self_reception` is not a trustworthy external TX witness.

The existing `#DRIVELOG` NVS journal holds counters, the first grouped
probe/failure and up to eight state/fault events. It **does not** contain
a complete per-transaction CAN TX/RX stream or connection generation for
every BLE request. Do not fabricate missing events or pass that journal
through the full bench checker as if it were continuous hardware evidence.

For a true isolated bench, collect all three independently:

1. Actual M5CAN Native stream and/or detailed instrumented session events,
   including BLE connection generation, request identity and delivery.
2. Raw bus capture from a **separate physical CAN analyzer**, converted with
   the offline `candump -L` importer when applicable.
3. Clock-sync evidence, externally measured drop counters, matching firmware
   hash, and independent QA signoff.

Until source-side request/peer instrumentation is available, the Native RX
import is *partial corroborating evidence*, not a substitute for full
`capture_events.jsonl`. No vehicle TX or firmware change is introduced by
this converter. All tests here use fabricated Native packets.

## Existing NVS DriveLog: sparse-journal audit only

When an operator has exported the complete `#DRIVELOG,BEGIN,...` to
`#DRIVELOG,END` serial dump with BLE devices disconnected, inspect it
**offline** without pretending it contains a complete frame timeline:

```bash
python3 tools/audit_m5can_drivelog.py \
  m5can_drivelog_original.txt drivelog_gaps.json
```

This audit verifies version-2 boot/CAN/TIMING/MEMORY records, TX
success/attempt count, the association and DLC/payload of PROBE/PROBE_RAW,
GROUPFAIL/GROUPFAIL_RAW and the last-8 EVENT/RAW records. It identifies
missing/truncated or explicitly SYNTHETIC probe evidence and nonzero driver/
BLE error counters, and preserves the input's SHA-256.

**It always reports `PARTIAL_JOURNAL_ONLY_NOT_HARDWARE_APPROVAL`.**
There is no complete CAN TX record, no per-request BLE generation and
delivery proof, and no independent CAN analyzer in this NVS journal.
Those omissions must remain visible even when all counters are zero.
It is invalid to reconstruct fictional unrecorded events just to make the
bench validator pass.

This tool never accesses NVS or serial hardware itself. It only processes
an already exported file. All host tests use synthetic textual fixtures.

### Existing BLE evidence journal (last twelve events)

Two-client firmware also exposes a **separate** `#BLELOG` journal,
persisted with up to twelve diagnostic events per boot and peer disconnect
summaries. Capture its complete `#BLELOG,BOOT,...` through `#BLELOG,END`
USB dump **only after peers have disconnected**; the firmware refuses
retrieval during active CAN queries or BLE peers. The existing USB text
commands are `DRIVELOG`, `BLELOG INDEX` and `BLELOG <boot_number>`
(or `BLELOG` to export the latest boot). Do not issue them while recording
CAN or while either BLE client is connected: the firmware will reject
active-traffic dumps. Save the exact `#DRIVELOG,BEGIN…END` and
`#BLELOG,BOOT…END` text sections as separate immutable files.

```bash
python3 tools/audit_m5can_blelog.py \
  blelog_original.txt blelog_evidence_gaps.json
```

The auditor validates v2/v3 journal schema, retained count versus ring
evictions, ordered event indices and generations, CAN result histogram,
disconnect summary and fault counts, and highlights lost events,
queue drops, storage errors or RAM-only snapshots. Its output always
states `SPARSE_BLE_JOURNAL_NOT_HARDWARE_APPROVAL`: the last twelve events
do **not** prove complete BLE notification delivery for each request,
nor every connection generation, and they cannot replace an independently
timestamped full CAN bus capture. It never invents discarded events.

Retain SHA-256 of the original USB dumps. For a multi-boot experiment
capture BLELOG separately per boot (or the indexed list first); a single
`BLELOG,BOOT` dump contains one chosen boot, not all historical boots.
The parser is offline only, and no new NVS writes are introduced.

## New RAM-only dual-BLE request correlation (explicit DRY profile)

To bridge the gap between Native CAN RX, DRIVELOG counters, and BLELOG
generation summaries, a **strictly TX-disabled** opt-in build is available:

```bash
pio run -e m5dial_dual_ble_trace_dry_bench
```

`M5CAN_BLE_DRY_TRACE_BENCH` has a C++ compile-time guard requiring BOTH
`M5CAN_DUAL_BLE_EXPERIMENTAL` and `M5CAN_CAN_MODE_DRY_RUN`.
It cannot be built with the physical TWAI transmit function enabled.
The ordinary M5Dial, multi-ECU policy and grouped CAN pilot profiles are
unchanged.

This bench-only firmware uses a bounded preallocated **128-event RAM ring**,
with no NVS writes, allocations or USB prints in BLE callbacks. It observes
BLE client connection `(connection_id, generation)`, accepted CAN owner
**queue submission** (not bus TX), CAN-owner completion/status, stale owner
rejection, BLE application-buffer queue accepted/rejected, and NimBLE
notification API accepted/rejected with peer generation. The CAN owner
token and read identity are only assigned to query-specific *application
queue* events; the lower-level NimBLE API does **not** claim that a given
notification chunk belongs to a specific request. Success of the NimBLE
API is **not radio-level acknowledgement from the peer**.

After disconnecting **all** BLE clients and stopping CAN queries, request
`DRYTRACE` on USB CDC and preserve the complete
`#DRYTRACE,BEGIN ... #DRYTRACE,END` section. The firmware refuses to dump
during active traffic. Overwritten ring records are counted and must never
be reconstructed.

```bash
python3 tools/audit_m5can_drytrace.py \
  original_drytrace_dump.txt drytrace_audit.json
```

The auditor detects reordered/reused generations, wrong ECU owner/result
associations, stale peer delivery-queue attempts, duplicate query tokens,
unexpected order, buffer overwrite and incomplete queries. It **always**
reports `DRY_RUN_SOFTWARE_TRACE_ONLY_NOT_VEHICLE_APPROVAL`, with
`physical_can_tx_observed=false`, `independent_can_analyzer=false`,
and `actual_ble_peer_receipt=false`.

The dry-run profile cannot validate actual CAN transmit latency or response
from a real ECU. Before real-vehicle qualification, the team still must
design a *separate*, independently reviewed bench-only physical TX
instrumentation with external CAN analyzer evidence, and obtain actual
peer-side BLE receive logs with request correlation and timestamps.
No change here authorizes physical multi-ECU transmission.

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
