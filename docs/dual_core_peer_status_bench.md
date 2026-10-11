# Dual-core M5Dial and per-peer BLE status (experimental)

Scope: `m5dial_dual_ble_bench` and `m5dial_dual_ble_mtu_bench` only.
Normal `m5dial` remains unchanged. This branch stacks on dual-compatible PR #13.

## Work allocation

- **CPU0:** Existing CAN/TWAI owner task (`m5can-owner`), with the only physical CAN TX path, ISO-TP reassembly, rate limiter, read-only validation and <=100ms positive-result cache.
- **CPU1:** NimBLE host (`MYNEWT_VAL_NIMBLE_PINNED_TO_CORE=1`) and new `m5can-ble-elm` protocol worker, FreeRTOS priority 4, 8192-byte stack. USB input, status print and LCD remain in the lower-priority Arduino task. No competing calls to `DualBleElm::poll()`.
- **Cross-core:** Existing fixed FreeRTOS request/result queues; nonblocking submit/read protected against USB vs BLE simultaneous callers. No CAN API calls in BLE callback context. BLE host callback only queues bytes; session parser and targeted notify run in the worker. The CAN owner still serializes *all* ECU queries.

The controller/host task core assignment and CPU load must be checked on hardware: compiled affinity is not evidence of latency improvement. ESP32 system tasks and interrupts may still run on either core; `CPU0 CAN/CPU1 BLE` refers to the primary application owners.

## On-device telemetry

The lower 4 rows of the **dual-connection bench** show per-peer `P1/P2`, BLE address last 4 hex digits, and **IN/OUT bytes per second**. IN = accepted BLE characteristic writes **from** that meter to the adapter. OUT = payload bytes enqueued as targeted notifications **to** that meter. `N` is cumulative queued notification count for this connection; *not* confirmed over-the-air deliveries.

The peer suffix identifies a BLE address; it does **not** reliably encode SmartRing vs JC3636. The operator should connect each device separately and record the suffix; random/private addresses can change. P1/P2 is a *connection slot*, not a permanent identity, and rate windows are reset on generation changes. Existing DEMO button and CAN frame/s remain active. Every 5s, USB emits `#BLE_PEER` with slot, address suffix, conn id, generation, MTU, cumulative RX/TX bytes, write counts, notifications, retries and failures. `#BLE_DUAL` still reports aggregate queue overflow.

This is **application payload throughput**, excluding GATT/ATT/link overhead; do not equate it with RF throughput. A bus with synthetic DEMO responses will have zero physical CAN frame/s and may have high BLE replies.

## Required hardware gate

1. Ensure USB-only, physical CAN/OBD disconnected, `DEMO AUTO active`, zero CAN RX/TX, SmartRing and JC3636 with compatible MTU247 fast-interval receivers. Keep the SHA-verified 65-minute stable 20B adapter image available for rollback.
2. Flash *only after CI succeeds on exact HEAD*. Start with one meter, then two. Record P1/P2 address mapping and `#BLE_PEER` every 5s. Compare each peer's counted delta-bytes/time against the LCD 1-second rate, reconnect one client and confirm stale generation cannot contaminate new peer stats.
3. Verify actual task core numbers `#CORE`, BLE negotiated intervals, FreeRTOS stack watermark, heap, watchdog state and GUI/touch response. Collect p50/p95 *per-peer* batch RTT and measured visible PID Hz. Compare 2×45s / 10min / 65min against PR88 baseline, especially JC 8Hz dip / rolling p95 123ms.
4. Abort and restore the tested 20B binary if CAN/OBD activity, BLE disconnect, missed reply, RX overflow, send failure, reset/watchdog, stack warning, meter staleness or p95 regression appears. No production merge or real-CAN testing until stable.

## Limitation
The host CI tests code paths and per-peer counters, not actual NimBLE task scheduling, LCD geometry or throughput performance. This PR does not promise zero latency. An observed sustained lag requires correlating per-peer application timestamps and radio connection events before further optimization.
