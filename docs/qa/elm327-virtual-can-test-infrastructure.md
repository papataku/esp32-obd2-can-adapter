# ELM327 conformance + PC Virtual CAN test infrastructure (cross-repo summary)

Date: 2026-10-28. **Design only.** Source-of-truth design: https://github.com/papataku/tacotaco-meter-ios/pull/25 (file: `docs/qa/test-infrastructure-elm327-virtual-can.md`).

## Owner decisions

- Existing reference device: KW905 only; no claim of owning a genuine ELM327.
- First implementation: complete CAN 11-bit and 29-bit; maintain an exhaustive ELM327 AT command expectations registry even for protocols deferred beyond CAN.
- Host: Mac ↔ M5Dial via a dedicated USB CDC Virtual CAN channel; **NO physical CAN access**.
- ECU model: PM/Codex parses the original RP8 SQLite strictly read-only and never commits it. Only a vetted JSON carrying PID, ECU response IDs, payload and relative times may be committed. The PM's already-created model requires schema alignment before ingestion.
- Acceptance hardware: iPhone 18 Pro + SmartRing + M5Dial + Mac, iPad later.
- Isolated serial virtual CAN only; no USB-CAN or SocketCAN in this phase.
- BLE acceptance: smoke first, then at least 30 min and 30 disconnect/reconnect cycles.

## Split and safety boundary

1. Docs PR (this PR): cross-repo summary and link, no source changes.
2. PC-only Python implementation on separate branch `test/elm327-conformance-sim`: ELM protocol emulator, synthetic ECU/ISO-TP, strict JSON fixture, Virtual CAN COBS/CRC codec and pytest shared suite with three selectable adapters.
3. Future, **separately authorized** firmware PR: pluggable CAN backend, dedicated Virtual CAN image with no TWAI install/start/transmit symbols, no CAN TX GPIO drive, no real CAN fallback; human-verified disconnected vehicle harness.
4. Separate future BLE/iOS acceptance: Mac CoreBluetooth or bleak + iPhone and SmartRing dual-Central evidence, not iOS Simulator-only.

No physical CAN transmit API or vehicle connection in this work. Even a green PC simulator suite is `SYNTHETIC` evidence, **not firmware, BLE hardware, or vehicle acceptance**. Existing test assets in `tests/`, PRs #34-#49, and V2 #58/#59 are reuse candidates; open PR changes are not considered merged.

## ELM source and conformance

Primary ELM327 datasheet: https://www.elmelectronics.com/wp-content/uploads/2017/01/ELM327DS.pdf .
External independent reference: https://github.com/Ircama/ELM327-emulator .
Keep CAN11/29, AT settings, syntax, prompt `>`, echo, CR/LF, spaces, headers, auto detection, ISOTP FF/CF/FC, error strings, ATST/ATAT, ATMA tests. Run the same declared test cases against PC emulator, M5Dial virtual CAN ELM mode (after its future implementation), and KW905 (only in an isolated non-vehicle test). Unsupported or NOT_RUN is not PASS. Preserve V1/ELM private extensions without misidentifying them as ELM327 standard behavior.
