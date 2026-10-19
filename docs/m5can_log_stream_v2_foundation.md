# M5CAN LOG STREAM V2 — foundation (bench-only, 2026-10-18)

## Status and compatibility
This is an **isolated first implementation**, not an enabled BLE log exporter.
The identical host-testable `m5can_log_stream_v2.h` lives in both
`papataku/esp32-obd2-can-adapter` and `papataku/TacoTacoMeter`.
Current `ATM5LOGREAD` remains 48 bytes per request, with unchanged
ELM/KW905 compatibility. Do **not** claim this commit accelerates deployed
devices, transfers real vehicle logs or validates MTU 247 on hardware.

The T4-S3 meter has a legacy-download 15-second no-progress guard in this
branch. It preserves the existing `.part` and adapter journal and releases
the PID pause; this bounds a stalled download but is not full persistent
resume. Meter polling requires a live BLE connection to produce new values.

## V2 notification framing
- ATT MTU=247 negotiated on **both** peers before V2 activation; never
  assume MTU=247 merely because `setMTU(247)` was requested. The
  20-byte legacy characteristic remains unchanged if negotiation fails.
- Max ATT value: 244 B. Header: 24 B; trailing CRC32: 4 B;
  **max DATA payload: 216 B**. ASCII hex is removed.
- Little-endian layout: magic `M5S2` (4), version (1), kind (1),
  reserved=0 (2), session ID (4), sequence (4), block (2), offset (2),
  block total (2), payload length (2), payload, CRC32 (4).
- Kinds: DATA, END, ACK. No v1 packet is accepted as V2. DATA requires
  a bounded 1..216-byte payload; END has no payload; ACK holds a
  durable next-sequence number plus block/offset cursor.
- `VerifiedCursor` advances only *after* independent persistent-storage
  validation by the caller; `WindowGate` permits at most 16 unacknowledged
  frames, i.e. <=3456 payload bytes before an app-layer ACK.
- On loss or timeout, rewind to the last durable ACK and reread the same
  immutable journal bytes; never silently skip gaps or auto-erase flash.
  Session/generation ownership, persistent resume after power loss,
  replay protection and full-archive hash verification still require
  the integrated transport and storage implementation.

## Next integration stages (NOT implemented here)
1. On M5Dial: add a versioned, authorized log-only GATT data channel,
   independently from the existing ELM UART, and a bounded flash-owner
   staging/ring queue. BLE worker may notify only a verified peer/generation
   and obey actual negotiated MTU, host queue credits, two-peer fairness,
   16-frame window and resend timeout. No flash work in BLE callbacks.
2. On T4-S3: subscribe to the log-only channel and enqueue notifications
   without doing SD I/O inside callbacks. A worker decodes frames,
   writes to `.part`, performs durable CRC/readback at checkpoints,
   then ACKs the verified cursor. Recover after reconnect without
   losing normal PID polling and the other BLE client.
3. QoS: keep RPM/SPEED/POWER poll freshness first; transfer sealed RAW
   log blocks in spare capacity. Never advance a flash reclaim pointer
   until a verified SD receipt; existing no-auto-reclaim remains default.
4. Host fault matrix: lost/reordered/duplicate/CRC-corrupt notifications,
   MTU23 fallback, MTU247 chunk boundary, peer generation change,
   flash read error, SD EIO, SD-full, power loss, reboot, delayed ACK,
   aggressive two-peer PID polling and 15-second no-progress recovery.
5. Physical acceptance: paired firmware/ELF hashes, exact actual MTU and
   connection intervals, notify/credit/mbuf counts, SD-byte CRC against
   adapter USB RAW copy, effective 10Hz RPM/SPEED/POWER, LCD frame time,
   20+ minutes, no unexpected reconnect/reboot/loss. Compare identical
   recordings with v1 vs MTU247 v1 vs V2 streaming. Real CAN is a
   separate, explicit hardware acceptance stage.

## Current tests and paired configurations
- Adapter host: `tests/compile_firmware_host.sh` adds
  `test_m5can_log_stream_v2.cpp` with AddressSanitizer/UBSan.
- Meter host: `tools/run_vehicle_offline_tests.py` adds the matching
  `test/test_m5can_log_stream_v2.cpp`, including legacy export timeout
  and 32-bit uptime wraparound checks.
- MTU A/B only: adapter `m5dial_dual_ble_multipid_mtu_bench` pairs
  with T4 `t4s3_m5can_mtu247_log_bench`. These have **no V2 streaming**.
  The T4 environment retains the isolated official SD driver fix,
  Core0 priority 2 and flush128 bench settings. Firmware builds and
  physical paired tests are still required; no automatic flashing.

Existing Draft PRs #32 (adapter) and #119 (meter) remain the parents;
new V2 foundation branches must not merge independently or modify
production default firmware.
