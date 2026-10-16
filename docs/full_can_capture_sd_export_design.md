# Full CAN capture, provenance, and BLE-to-meter SD export — design proposal

Status: DESIGN ONLY. Main implementation issue: adapter PR #32 (open draft).
Meter counterpart: TacoTacoMeter PR #119 (open draft).
Date: 2026-10-16. Do not claim full capture, in-vehicle support, SD export,
or physical acceptance merely because firmware builds.

## User requirements and baseline

- Adapter M5Dial HAS NO microSD. Its CAN logging and bounded RPM+speed
  read-only multi-PID request implementation must be compiled into the
  normal selected firmware configuration; no end-user compile feature-off
  choice. Start and stop are **session controls**, not an optional-feature
  enable/disable toggle. Recording starts OFF on every boot.
- SmartRing, JC3636W518C, and LILYGO T4-S3 meter builds share one session
  controller and SD file writer; T4-S3 has an onboard **SPI** microSD:
  MISO=GPIO4, MOSI=GPIO2, SCK=GPIO3, CS=GPIO1, not the
  SmartRing/JC SDMMC wiring. The paired meter PR #119 implements the
  always-compiled T4-S3 SD baseline; it does not implement BLE download.
- User START -> both adapter and meter begin the same session; STOP/SAVE ->
  both stop and flush; after STOP the meter suspends PID polling and gauge
  animation, retains BLE and SD writer, downloads adapter evidence to SD.
- Requested recording scope is **every classical CAN frame that the M5Dial
  TWAI controller actually observes**, not just RPM and not just OBD
  responses. Add every app-submitted diagnostic/ISO-TP flow-control TX
  attempt separately. Preserve usable timestamps and loss indicators.
  Never claim to capture frames the controller missed or CAN error frames
  that the ESP32 TWAI API does not provide as data-frame records.

## Three complementary, versioned streams

1. Raw CAN stream (losslessly reconstructable): adapter monotonic capture
   timestamp, arrival-order sequence number, 11/29-bit arbitration ID,
   IDE/RTR/self flags, DLC, 0..8 payload bytes, and source:
   CAN_RX, CAN_TX_SUBMIT_OK or CAN_TX_SUBMIT_ERROR. A successful TWAI
   submit is not proof of exact physical on-wire arbitration/completion
   time; preserve this distinction. Clock measurements are taken in
   firmware after twai_receive or around twai_transmit, not at the ECU
   internal sample or an external CAN hardware timestamp.
2. Diagnostic/provenance stream: acquisition ID, original CAN responder
   IDs and physical RAW links, 01 0C 0D grouped vs individually requested
   PID, any NRC/timeout, ECU selection, CAN-owner cache and coordinator
   cache, BLE peer/generation. RPM 450..550 to 0 transitions and repeated
   same-value intervals should remain queryable without inventing ECU
   sampling timestamps. Existing Phase-A RpmRunCompressor is an indexed
   derivative, not a replacement for the RAW stream.
3. Session/health stream: firmware identity, session ID and boot ID,
   start/stop acknowledgments, storage capacity, uncommitted block loss,
   session counter wrap, missing sequence ranges, TWAI driver RX missed
   and overrun, app queue drops, TX errors, RAM/flash overflow, flash
   write/erase latency, BLE transfer retries, resulting completeness.

Absolute "all frames" is a **verified quality state** (COMPLETE only if
all relevant loss counters are zero). If overrun occurs, mark INCOMPLETE
with a sequence gap; do not silently subsample or represent cache
deliveries as fresh CAN receptions. For full-bus capture, TWAI filters
remain ACCEPT_ALL; the logger itself never generates new diagnostic
requests, relaxes TX leases, or switches the CAN controller mode.

## Capture hooking and non-blocking ownership

Existing CanMonitor::captureMessage is used at three receive paths:
normal idle receive, 50ms diagnostic pacing waits, and in-flight
diagnostic/ISO-TP transactions. It currently enqueues CapturedFrame into
the existing frame_queue_ (bounded to 512) and counts drops; the new
recorder must branch independently before losing information if the
application consumer falls behind, without causing extra RX copies or
heavywork inside CAN owner. Emit a bounded copy of each RX to a
dedicated fixed-capacity SPSC recorder ring (CAN owner producer,
low-priority encoder/flash consumer). Do not tie recorder progress to
the existing GUI/USB frame_queue_ draining policy. TX hooks must be
placed at transmitOwnedFrame or its underlying success/failure point:
record exactly one submission event per application TX, including
functional diagnostics and ISO-TP flow-control frames, not duplicates
when cached values are delivered. Do not write flash, allocate memory,
format strings or notify BLE from any TWAI/IRQ/GATT callback.

The encoder consumes ring batches and reserves CAN-idle checkpoints
for flash erase/program so diagnostics never overlap flash mutation
without an explicit measured safety gate. Measure whether the 256-item
TWAI driver RX queue masks worst-case erase latency; do not assume it
does. If capture falls behind, report exact drop counters and timestamps
and label incomplete. Device continue serving meters or report storage
failure without starting new vehicle CAN TX.

## Compression (reversible for RAW, separate from lossy RPM summary)

Do not discard repeated raw frames or reduce every timestamp to a
representative interval. Proposed bounded CPU-first transform:
- block header: protocol version, session/boot ID, first/last raw
  sequence, timestamp base, record count, original byte count, codec ID,
  encoded byte count, CRC32 and durable generation/commit status;
- per-block/dynamically bounded CAN-ID dictionary, variable-length
  signed/unsigned timestamp deltas, flags+DLC packing, previous-payload
  XOR and compact changed-byte masks when useful; a raw literal escape
  for non-compressible frames. Exactly reconstruct original order,
  ID, DLC, flags, payload bytes and recorded timestamps;
- optional LZ4/heatshrink only after memory, CPU, flash throughput and
  compression A/B measurement; do not commit blindly to a CPU-heavy codec;
- independent ~2 KiB payload blocks under current 2304-byte blob
  ceiling where applicable; CRC and commit-last per block; dictionary
  reset boundaries allow random/resumable BLE transfer and independent
  corruption recovery.

This is an **initial format proposal**, not a codec already available
in the adapter. Preserve existing EvidenceStore owner metadata, first
20 protected keys and unrelated diagnostic NVS; do not mount/format or
overwrite foreign data. Add new type/slot mapping, retention/index
metadata, recovery and rollback tests before touching physical flash.
Avoid storing huge per-frame indexes in SRAM.

## Capacity reality and required loss policy

Measured user RP8 log in issue #23: approximately 97,598 CAN RX frames
and 36,949 physical TX over 1,615.5 seconds, or ~83.3 combined
events/second (60.4 RX/s and 22.9 TX/s). With an illustrative uncompressed
24 bytes per stored event this is ~2.0 KB/s, ~7.2 MB/hour, and an
otherwise-empty 1.5 MiB flash region lasts only about 13 minutes. This
is a worked budget, NOT measured compressed output or available free
space. Different buses/loads may be much higher; a 500 kbps bus is not
a promise of a constant frame frequency or long-term flash capacity.

Lossless compression increases retention only by the measured ratio.
For example, to hold an entire 41-minute test at that observed event
rate in 1.5MiB, the average encoded cost must be below ~7.6 bytes/event
even before protected metadata and flash overhead. Neither that ratio
nor a guaranteed retention time has been demonstrated.

v1: bounded internal flash capture; capacity guard before accepting
BEGIN; when full, seal a COMPLETE_UNTIL_FULL / INCOMPLETE_SESSION
manifest and stop RAW capture with explicit error. Do not silently
overwrite prior unrecovered sessions or existing evidence; preserve
valuable diagnostic summary and current state. Recover the existing
closed blocks via meter BLE after STOP.

If truly complete hours-long sessions are required, an **additional
validated streaming-spill mode** must progressively transfer *sealed*
blocks from adapter flash to the paired meter SD *during recording*,
with independently ACKed, durable SD readback and nonblocking BLE
traffic budgeting. Only then can safely reclaimed adapter blocks be
reused. It requires demonstrated dual-meter polling, BLE notification
credit, SD mount/write/readback, flash erase and reconnection stability.
Do not claim the after-STOP-only flow can guarantee unlimited logs.

## Transfer protocol and SD acceptance

Use a versioned M5CAN-specific GATT log control/status/data plane, not
the legacy ELM prompt or RX/TX text stream. LOG_CAPS, LOG_BEGIN, LOG_END,
LOG_STATUS, LOG_LIST, LOG_READ(session_id, offset, size), ACK and ABORT,
with authenticated session owner and correct conn_id/generation.
BEGIN/END are idempotent via transaction ID; a disconnect or power
cycle must not manufacture a successful STOP/COMPLETE.
One active capture and one exporter; other active peers must not be
disconnected or silently starved. Return BUSY for export when other
meter still actively polls unless an explicit safe coordination policy
has been implemented.

For an after-STOP export, suspend the exporting meter's PID polling,
suspend gauge animation, retain a low-rate progress UI and keep the BLE
connection and SD writer operating. Capture only a frozen, immutable
session index. Chunk length/offset/record hashes, CRC, read-ahead credit,
app-layer ACK/retry and session-resume offset are required even if BLE
reports a successful notification enqueue; enqueue is NOT durable SD
acknowledgment. Persist `adapter_can.bin.part` and a sidecar manifest
with firmware, boot, wall-clock estimate/monotonic sync samples,
received byte count, block count, corruption and gap counters. After
flush, readback, whole-file hash/index verification, atomic rename
and meter-side persisted confirmation, allow explicit receipt ACK;
**never automatically erase adapter flash** on BLE delivery alone.
Saved raw CAN and RPM/derived analysis must use distinguishable files.

All three SD targets share the same session/transfer writer above HAL:
SmartRing uses verified SDMMC HAL, JC experimental SDMMC HAL (physical
readback outstanding), T4-S3 official SPI pins noted above (physical
readback outstanding). Power cycles always boot with recording OFF, but
completed prior adapter sessions remain discoverable.

## Tests / acceptance before any real vehicle use

1. Deterministic host feed: raw 11/29-bit, RTR, DLC0..8, duplicated
   frames, same-value timings, ISR/backlog synthetic bursts, TX failures,
   ISO-TP concurrent response path; exact binary reconstruction and
   counters across block/sequence/timer boundaries.
2. Compression A/B over representative 41-minute RP8 raw session, plus
   high-entropy/high-bus-load fixtures. Publish mean/peak CPU time,
   ratio and bytes/s, RAM and stack. If source has only meter-level
   data and no actual entire CAN bus dump, say full-bus compression
   ratio is unknown; never substitute invented frames.
3. Storage fake with power cut during erase/header/payload/commit and
   checksum corruption, allocator wrap, session collision, protected
   EvidenceStore keys, foreign partition and rollback; verify
   no unannounced data destruction.
4. BLE MTU23/247, two peers, lost/duplicate/out-of-order chunks,
   client reconnect and power cut, invalid owner, no SD, full SD,
   throughput under realistic 10Hz foreground meter polling.
5. Hardware TX-disabled DEMO, then isolated simulated ECU bench:
   record/STOP/export SD readback, verify no unexpected CAN TX,
   drop counters and flash pause impact. Only after evidence and
   user-approved physical vehicle test assess the full frame scope.
