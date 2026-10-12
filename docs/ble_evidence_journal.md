# M5CAN persistent BLE diagnostic evidence

This is an opt-in diagnostic addition **stacked on PR #26**. The existing
CAN DriveLog and PR #25's grouped-PID NVS probe/failure snapshots remain
independent. The goal is correlation, not changing Bluetooth or CAN timing.

## Collection

- PR #26 owns the NimBLE GAP observer, its 64-slot callback ring and the
  unchanged BLE client/generation routing. The Arduino main loop drains up
  to 8 diagnostic events per pass.
- This journal copies those events to a fixed 24-event RAM record: connect,
  GAP disconnect and rejected peer; raw NimBLE host reason integer (not
  necessarily HCI), link/peer generation, negotiated MTU as observed,
  interval, latency, supervision and cumulative per-peer byte/retry/failure
  counters. Sequence gaps and dropped queue events are counted separately.
- A distinct NVS namespace, `m5can_blelog`, contains keys `ble0..ble7`,
  keyed by the **same boot sequence as DriveLog**, each with magic,
  record-length/version, FNV-1a integrity checksum and source build ID.
  Only the latest 24 events for each of the last 8 recorded boots survive.
  This is **not** a continuous raw BLE packet capture, and does not claim
  per-notification acknowledgement, RSSI, PHY or full MAC identity.
- Flash writes are done only by the Arduino loop, after an existing
  `CanMonitor::tryBeginLogCheckpoint()` CAN-idle reservation, never from
  BLE callbacks. Disconnect/reject is urgent, but writes have a minimum
  5-second interval after a prior success, with 30 seconds for ordinary
  new connect events; failed writes retry no faster than every 2 seconds.
  No per-packet NVS writes. A sudden power loss before an idle checkpoint
  completes can lose the newest RAM-only events.

## Export after reproduction

1. Finish the capture, disconnect active BLE peers and make sure no CAN
   query is in flight. Stop at a safe stationary location; don't perform
   a serial dump while driving.
2. Connect the adapter to a USB host, taking care that opening the port
   can trigger a USB reset. This **does not clear** the prior boot's NVS.
3. Send `BLELOG INDEX` to list available boot sequences.
4. Send `BLELOG <boot>` to get one boot's details; plain `BLELOG`
   returns the latest retained record. Save all lines through
   `#BLELOG,END`. If peers or CAN requests are active, export returns
   `#BLELOG,ERR,ACTIVE_TRAFFIC` instead of monopolizing serial output.
5. Send existing `DRIVELOG` separately for CAN, resets, memory and,
   after PR #25 integration, `PROBE_RAW` / `GROUPFAIL_RAW`. Join by
   `boot` + monotonic `ms`; align with iPad/SmartRing UTC using independent
   clock-fit evidence and report uncertainty.

If `queue_drops>0`, `seq_gaps>0` or `evicted>0`, the BLE trace is
incomplete; do not conclude that absent connect/disconnect events never
occurred. `reason_known=0` means unknown, not a successful connection.
The low 16-bit peer suffix is diagnostic only, not a stable identity and
not proof of who initiated a disconnect.

## Merge order and validation

The branch is based on PR #26's `diag/issue21-tri-device-soak` head;
base the persistence PR on that branch to keep the active disconnect
diagnostics separate from PR #25. Do not merge either unfinished draft
directly into main. A follow-up integration pass must verify PR #25's
NVS probe snapshots and this BLE journal remain independently readable
after both merge, with no added CAN TX or same-peer cache changes.

Host regression: `tests/test_ble_evidence_journal.cpp` checks cold-boot
record reload, raw disconnect reason, CRC corruption, 24-event wrap, callback
queue loss, write throttling and CAN-owner checkpoint blocking. PlatformIO
and stack budgets also remain gates. **Physical power-cycle/USB recovery**
and concurrent iPad+SmartRing load must be completed before accepting
real-device retention.

## Classified error routes (software review)

All known error branches in the current dual-peer diagnostic pipeline are
either recorded at their source or represented by monotonic failure counters.
The word *all* is scoped to the listed, observable software routes. An unknown
radio-controller failure cannot be assigned an invented NimBLE cause.

| Route and original source | Persistent classification | Evidence |
|---|---|---|
| GAP disconnect, peer rejected, no slot | `BLELOG EVENT`; `101` no-slot | raw NimBLE reason, handle, peer generation, MTU, link interval, bytes |
| BLE callback RX queue full | `102` + `521` | per-event + global dropped byte counter |
| Notify retry/backpressure, definitive failure | `103` / `104`, `524` / `522` | retry count; raw NimBLE host error as `detail`, counter deltas |
| GATT stale connection/generation / asynchronous result to old peer | `105` / `404` | **old** generation; no invented new peer identity |
| GAP diagnostic ring drops | `106` | total lost and stream sequence gaps |
| Read result (NoData, Busy, lease, policy, rate, driver, BusOff, mode, ISO-TP) | `201..209` | specific `DiagnosticStatus` and transaction duration, not just aggregate `CAN ERROR` |
| Grouped PID rejected/stopped or lost peer | `301..303` | CAN status, response count, raw-frame count; PR #25 will add ECU-selection *reason* and original CAN evidence |
| ELM pending command, too-long input, parser error, old peer result | `401..404` | per-session error, not a global reset |
| CAN health counters | `501..508` | timeouts, query failures, queue drops, TWAI RX missed/overrun/bus error/arbitration; init failure |
| BLE transport counters | `521..524` | dropped writes, notify failure/retries, session rejections |
| Initialization and task creation | `107`, `405`, `508` | boot sequence and build SHA; log through degraded main loop |
| Journal NVS errors | `901`, `storage_errors` | RAM error record; recoverable only if a later NVS write succeeds |

The persistent record retains the most recent 24 individual link/errors for
each of eight boot epochs, plus 48 **independent error-category counters** with
first/last monotonic timestamps and first/last detail values. Global source
counters are sampled every five seconds, and their actual delta—not merely one
per poll—is accumulated. Therefore a burst that overwrites individual event
entries can still leave an attributable count. The `unclassified`,
`queue_drops`, `seq_gaps`, `evicted` and `storage_errors` fields explicitly
disclose data loss. A 48-bucket limit, 8-boot retention and power-cut-before-
checkpoint remain bounded failure modes.

Failure evidence != causal proof. For example, `BLE_NOTIFY_FAILURE` plus
a subsequent HCI supervision timeout is correlation, not proof that GATT
backpressure caused the RF link to drop. Distinguish expected `NO DATA`
for unsupported DID probes from unexpected CAN timeouts in the report.

### Integration not yet covered by this stacked PR

This branch is **not** a merge of #25 and #26. It preserves #26's live BLE
failure path and adds its own NVS sidecar. When #25 lands, the integration
pass must add grouped ECU choice/fallback reason, grouped RAW-frame timestamps
and status to a shared offline timeline export, and verify both journals
survive a physical cold boot without overwriting one another. Tests must
also check CAN stack budget and CPU1 BLE latency after NVS commits.
