# Project Status

Last updated: 2026-09-29

## Current baseline

`main` is intentionally at **Phase 1: Listen-Only RAW CAN monitor**.

Implemented:

- M5Dial / ESP32-S3 PlatformIO target
- M5Stack Mini CAN Unit U179 pin baseline (GPIO13 TX / GPIO15 RX)
- Classical CAN 500 kbit/s
- TWAI `LISTEN_ONLY`
- TWAI TX queue disabled (`tx_queue_len = 0`)
- no `twai_transmit()` path in firmware
- RAW standard/extended/RTR frame capture
- microsecond timestamp at firmware receive path
- RX/drop/driver error counters
- M5Dial status display
- USB text stream and Mac capture script
- host static/syntax CI
- real PlatformIO M5Dial build CI

## Verified

Software gate:

- host translation-unit build with warnings-as-errors: PASS
- Phase 1 static no-TX safety checks: PASS
- text frame format test: PASS
- GitHub Actions host job: PASS
- GitHub Actions PlatformIO M5Dial build: PASS

Latest build-fix commit at this checkpoint: `aac23bc`.

## Hardware validation still required

Do not mark Phase 1 complete until the following are checked on hardware:

1. U179 termination configuration is measured/understood.
2. M5Dial boots and reports `LISTEN_ONLY` with CAN disconnected.
3. Bench or vehicle CAN reception at 500 kbit/s is confirmed.
4. No unexpected CAN TX is observed.
5. `drop`, `driver_missed`, and `overrun` remain zero under representative load.
6. USB log can be captured on the Mac and replayed/inspected.

## Development policy while hardware validation is pending

Phase 2 Native protocol work may be developed on a feature branch because it remains receive-only. It should not introduce CAN TX and should not be merged as a completed Phase 2 baseline until its own tests are green.

Phase 3 diagnostic TX work must not be merged or exercised on a vehicle until the preceding safety gates are satisfied.
