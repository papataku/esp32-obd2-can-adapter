# Multi-ECU UDS22 — five-discipline review and release gates

Status: **AI-conducted discipline-separated desk review**. This is **not**
independent approval from five real specialists. Owners / qualified reviewers
must be assigned before any physical CAN bench or vehicle TX approval.
Date: 2026-10-18. Scope: adapter Draft PRs #34, #35, #37 and #38;
iPad Draft PRs #15 and #16.

## 1. CAN / ISO-TP transport reviewer

Evidence reviewed:
- fixed normal 29-bit request `18DAxxF1` ↔ response `18DAF1xx`;
- only declared physical source bytes 01/02/06/0E/EF in **TX-disabled build**;
- `0x22` ReadDataByIdentifier only, rejection of other physical services;
- wrong source FF/CF, wrong DID, NRC31, NRC78, FF overflow, CF timeout and
  sequence/collision in deterministic host simulation.
- PR #38 rejects Classical CAN `dlc > 8` before reading/copying eight-byte
  arrays and rejects truncated negative replies `7F 22`.

Open for real expert sign-off:
- independent socketCAN/vcan or CAN simulator replay using observed IDs,
  wrong source, concurrent tester, CF timing, ENOBUFS, and exact FC TX capture;
- ECU response source identity under actual TWAI interrupt/frame delivery.

## 2. Embedded vehicle safety reviewer

Confirmed in source-level review:
- normal release stays physical ECU01-only; no wildcard header;
- opt-in experimental addresses require the separately selected
  `M5CAN_MULTI_ECU_POLICY_DRY_BENCH` *and*
  `M5CAN_CAN_MODE_DRY_RUN` compile flags;
- experiment does not install TWAI Normal mode: Listen-Only only, no active ACK
  or CAN TX; negative compilation test for missing TX-disabled flag.

Unapproved:
- an **independent** bench-transmit profile, lease lifetime/rate cap,
  CAN BUS-OFF and no-auto-rearm review, vehicle parked/P validation,
  and an emergency stop/physical isolation plan.
- No user should flash a draft as an ECU02/06/0E/EF production reader.

## 3. BLE / RTOS concurrency reviewer

Checks:
- single CAN owner and at most one outstanding request;
- two BLE peers can ask different ECU source addresses for the same DID;
- outputs are routed by connection ID **and generation**;
- stale owner results cannot be applied to a reconnected handle;
- retained same-PID secondary result now also requires exact original
  `can_id/service/PID-or-DID` identity (PR #38);
- handle-reuse, lost peer, same-parameter fanout, scheduler adaptive and stable
  modes, no cross-peer result leakage tested.

Open:
- real 2-device BLE soak, starvation/fairness, controller credits and
  asynchronous GAP disconnects under timing jitter; measure stack high-water,
  queue watermarks, heap, p99 latency, notify drops.

## 4. iPad / SQLite integration reviewer

Checks:
- existing `M5CAN PROTO=1.1` never authorizes new physical ECU addresses;
- `ATI` M5CAN identity remains restrictive even when capabilities fail;
- optional future `UDS_ECUS` is parser-only and NOT a host TX enable;
- never equate adapter `?\r>` with vehicle NRC31 or DID not supported;
- park confirmation must be explicit, and speed 0 must be fresh/stable.

Open:
- joint golden trace with real adapter firmware SHA, `ATM5CAP` and
  reconnect/version renegotiation; ensure `did_drive_plan` and actual sample
  provenance persist through interruption and SQLite migration.

## 5. QA / reproducibility reviewer

Gate order:
1. All host and sanitizer regression tests pass, including malformed
   DLC=9/255, truncated NRC, combined dual BLE/UDS session replay.
2. All production + opt-in TX-disabled PlatformIO environments build;
   compiler stack-frame budgets pass.
3. Isolated CAN simulator with measured TX/FC, bitrate and termination,
   without connection to a live vehicle.
4. Two-device BLE and isolated CAN endurance run with raw trace and failure
   replay, no false positive or stale-session data.
5. Named **human** review sign-off by all five disciplines.
6. Separate protocol and safety-approved feature PR to enable *one specific
   physical ECU at a time*, then controlled parked-vehicle read-only trial.
7. Only after bench evidence: merge/release decision and RPM field validation
   across independent EV/engine-on/regenerative sessions.

No human sign-off, no isolated CAN hardware validation and no live vehicle
validation have been completed as part of this document. Keep all related
PRs as Draft until the appropriate gates are passed.
