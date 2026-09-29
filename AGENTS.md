# AGENTS.md

このファイルは、このリポジトリを扱うChatGPT/Codex/その他Coding Agentが**毎回共通して理解しておくべき前提**を記録するためのものです。

## Project identity

- Repository: `papataku/esp32-obd2-can-adapter`
- Working product/firmware name: **M5CAN**
- Goal: ESP32ベースのELM327互換 + RAW CAN/ISO-TP/OBD-II/UDS解析アダプタ
- Initial hardware: M5Dial (ESP32-S3) + M5Stack Mini CAN Unit U179
- Initial validation vehicle: Honda STEP WGN e:HEV RP8
- The repository must remain reusable for other ESP32 boards and vehicles where practical.

## Non-negotiable safety rules

These rules take priority over feature speed.

1. Device boot state MUST be CAN receive-only / Listen-Only.
2. CAN TX MUST be disabled by default.
3. Diagnostic TX MUST require an explicit, time-limited TX Lease.
4. Expired lease MUST become logically invalid immediately; do not rely only on a later polling loop.
5. Host disconnect, lease timeout, BUS-OFF, mode-switch failure, or unrecoverable error MUST move toward TX-disabled state.
6. Do NOT automatically resume diagnostic TX after BUS-OFF recovery.
7. Do NOT expose an unrestricted raw-CAN transmit command/API during early phases.
8. Until explicitly expanded, allow only reviewed read-only OBD/UDS request forms.
9. Mode changes between Listen-Only and Normal must be synchronized with all TWAI RX/TX users. Never uninstall/reinstall/reconfigure the TWAI driver concurrently with a receive task.
10. A received frame is not assumed to belong to our diagnostic request merely because the CAN ID matches. Match request/response context before sending ISO-TP Flow Control.
11. Preserve RAW CAN independently of higher-level decoding whenever possible.
12. Never advance a development phase merely because code compiles. Follow the acceptance gates in `docs/ROADMAP.md`.

## Hardware assumptions

Initial target wiring:

```text
M5Dial PORT.A              U179
GND       ---------------- GND
5V        ---------------- 5V
GPIO13    ---------------- CAN_TX
GPIO15    ---------------- CAN_RX
```

Vehicle side:

```text
OBD Pin 6   -> CAN-H
OBD Pin 14  -> CAN-L
OBD Pin 4/5 -> GND
OBD Pin 16  -> NOT USED
```

Power:

- M5Dial is powered from an ACC/IG-switched USB 5V source.
- U179 is powered from the M5Dial side.
- U179 HPWR+ is not used.
- Do not power the adapter from OBD Pin 16 in the normal vehicle configuration.
- M5Dial battery operation is not part of the normal vehicle configuration; power-off behavior must remain deterministic.

U179:

- Mini CAN Unit U179
- TJA1051T/3 CAN transceiver
- non-isolated
- its onboard 120-ohm CAN termination must be considered explicitly.
- Before permanent vehicle connection, measure the vehicle bus and adapter termination. Do not blindly add a third 120-ohm termination to an already terminated bus.

## CAN baseline

Initial bring-up target:

- Classical CAN
- 500 kbit/s
- 11-bit standard OBD functional addressing first
- 0x7DF functional request
- 0x7E8-0x7EF typical OBD responses

29-bit CAN and Honda-specific UDS are later phases and must not be mixed into basic bring-up until the lower layers are stable.

## Layering rule

Keep these responsibilities separated:

```text
TWAI driver
  -> RAW CAN core
      -> raw logger / native stream
      -> ISO-TP
          -> OBD-II
          -> UDS
          -> ELM327 compatibility
```

Do not implement ELM327 parsing directly against the TWAI driver.

RAW CAN is the source of truth. Higher-level protocol bugs must not destroy the ability to inspect what was actually received.

## Host transport

Preferred development/analysis transport is USB.

Native protocol goals:

- binary framing
- sequence numbers
- integrity check (CRC)
- raw packet capture before semantic decoding
- replayable logs

The current design direction is COBS + CRC32, but if this changes, update `docs/DECISIONS.md` and protocol documentation together.

BLE/Wi-Fi may be added later, but must not force the core CAN architecture to depend on them.

## Logging

- High-volume full RAW CAN logs belong on the host (Mac), not continuously in ESP32 flash.
- ESP32 may keep a bounded RAM ring buffer for recent events.
- Flash writes should be limited to configuration and intentionally captured fault/event records.
- Logs should make it possible to distinguish:
  - CAN RX/TX
  - ISO-TP state
  - OBD/UDS request-response
  - lease state
  - BUS-OFF/errors
  - host commands
  - dropped frames / sequence gaps

## KW905 role

KW905 is a Golden Reference, not the final architecture.

Use it for compatibility/performance comparisons:

- identical OBD requests
- payload equality
- latency P50/P95/P99
- throughput
- timeout/error rate
- jitter

Do not copy undocumented KW905 behavior blindly if it conflicts with safety or protocol correctness.

## Development discipline

Before significant code changes:

1. Read this file.
2. Read `docs/ARCHITECTURE.md`, `docs/SAFETY.md`, and `docs/ROADMAP.md`.
3. Check `docs/DECISIONS.md` for prior architectural decisions.
4. Identify which phase the change belongs to.
5. Keep the phase scope narrow.

For every phase:

- host-side unit tests
- parser/protocol boundary tests
- static checks for forbidden TX paths where applicable
- PlatformIO build on the actual target configuration
- bench CAN test before vehicle TX testing
- vehicle testing starts with read-only operations

When a discovered bug changes an architectural assumption, document it in `docs/DECISIONS.md` or the relevant safety/architecture document.

## Important prior review findings

These are known classes of bugs and must not be reintroduced:

- RX task racing TWAI driver stop/uninstall/reinstall during mode switching
- lease expiry not taking effect until a later poll
- arbitrary payload allowed through an apparently “OBD-only” command
- Flow Control sent to another diagnostic tool's ISO-TP conversation
- stale queued frames accepted as a new query response
- query timeout checked before draining already-received frames
- unbounded host input delaying safety-service execution
- TX queue/retry behavior causing unintended repeated diagnostic traffic

Treat these as regression-test targets.

## Related projects

The Mac Honda e:HEV Analyzer is a separate project. This adapter should provide a clean transport/API so the analyzer can support both:

- KW905 / ELM-compatible transport
- M5CAN Native transport

Do not put vehicle-specific signal-analysis UI into this firmware repository.
