# Two-meter disconnect cause debugging (2026-10-13)

Scope: M5Dial adapter + SmartRing + JC3636W518C; iPad excluded. Firmware and complete physical evidence are recorded in `integrated_physical_evidence_20261013.json`. No new physical run is represented by the host fault injection below.

## Confirmed causal path

Both physical failures occurred on SmartRing conn=2. Notify failure code104/detail6 (BLE_HS_ENOMEM) precedes raw GAP reason534 (0x216, local termination) by51ms and402ms respectively. Adapter boot47 did not reset. `BleElmTransport::writeTo()` explicitly calls `server_->disconnect(id)` after an incomplete write. This explains the observed disconnect rather than a supervision timeout.

The 100ms retry deadline starts at entry to the entire `writeTo()` call, not at the first allocation failure or each chunk. Successful chunks and their delays consume that budget too. Therefore the log does not prove that either physical failure experienced 100ms of continuously unavailable memory. Do not equate cumulative per-peer retry counts228/5172 with retries for the failed individual write.

Physical telemetry around the cuts (5s reporting windows):

| UTC observation | BLE poll peak us | UI loop peak us | Total retries | Failures |
|---|---:|---:|---:|---:|
|16:54:36.306557|19182|28485|38|0|
|16:54:41.338648|100250|114132|228|1|
|17:17:36.401296|99870|28260|5183|1|
|17:17:41.421298|100248|120022|5400|2|

These are window maxima, not precisely timestamped function traces. They support the blocking-send mechanism but cannot establish whether a concurrent flash write triggered a particular allocation failure.

## Reproduced with production transport code

`tests/test_dual_ble_transport.cpp` now injects ENOMEM into the real `writeTo()` implementation. The NimBLE host stub records requested disconnect handles:

-99 failed attempts followed by success: returns one byte at99ms, requests no disconnect.
-ENOMEM through the100ms deadline: returns zero bytes, records code103/detail100 and code104/detail6, requests disconnect only for the affected handle.
-The other connected peer still accepts a subsequent write. The synchronous caller is nevertheless occupied during those100ms; this can delay servicing the other session.

The existing runner checks both20-byte and opt-in244-byte profiles. This isolates application policy, not radio or real controller behavior.

## What existing evidence cannot distinguish

The ternary expression combines two ENOMEM sources: `ble_hs_mbuf_from_flat()` returning null and `ble_gattc_notify_custom()` returning6. The vendored NimBLE implementation consumes/frees the supplied mbuf on the notify path; the caller must not add a second free as a speculative leak fix. ACL/controller queueing is bounded; the dependency defaults define MSYS1 blocks12 and ACL buffers12, but compiled runtime capacities and actual exhaustion must be measured before assigning blame to either pool. General free heap alone cannot prove availability in a dedicated pool.

The first-connected JC peer had zero retries in these physical intervals, while SmartRing retries grew and reset after reconnect. A reverse-order5min run had no failures; reverse-order high-request10min run had6518 retries without a cut. These durations do not exclude a long-duration order effect. Higher requested PID rates produced approximately10Hz actual fast-PID rates and do not reproduce the original iPad exploration load.

## Required next instrumentation and acceptance

Implementation handoff (no production transport change in this debug commit):

1. Record a distinct failure stage (mbuf allocation vs notify API), per-write bytes requested/sent, chunks, elapsed time, first-error time and retries. Sample free MSYS blocks on the first error and on give-up. Capture bounded scalars in RAM; do not print or write flash from BLE callbacks.
2. Timestamp journal erase/write start/end and slow operations using the same adapter clock; correlate with the first allocation failure. Preserve first failure + subsequent GAP event before detail FIFO eviction.
3. Run equal-duration normal trials in both connection orders, then change one variable at a time: existing fast connection interval profile, or a RAM-only logging diagnostic profile. Verify actual connection interval and effective meter PID rates. Keep CAN TX disabled and iPad excluded.
4. Evaluate bounded per-peer pending output with resumable/fair servicing. Preserve ELM response/prompt ordering and reconnect generation. Raising the synchronous deadline alone extends the delay imposed on the other peer.
5. Confirm two peers over at least2h at the relevant achieved workload, with zero unexpected cuts and explicit pending-output overflow/recovery evidence. Do not merge on the host injection result alone.

The local disconnect trigger is established. The physical resource bottleneck remains unresolved; no new firmware was flashed and no main branch was merged during this debugging pass.
