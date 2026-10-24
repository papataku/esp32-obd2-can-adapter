# Issue #54 — Physical ECU selection, design v1 (2026-10-24)

Status: **opt-in lab prototype only**, not an approved vehicle firmware. Listen-Only RAW signal decoding tracked separately in #55.

## Algorithm / state
- On genuine BLE RPM + speed grouped demand, enter Discover: 8 bounded functional `18DB33F1:01 0C 0D` probes, never automatic TX on boot. No timer-only polling.
- Candidate: an ECU must return complete and RAW-backed paired `41 0C xx xx 0D xx` (either PID order) for every *reply it sends*. Partial/malformed/duplicate replies disqualify it. Missing replies lower success count out of eight.
- Deterministic order: successful probes, observed changes (RPM **or** speed), lower average response latency, smaller CAN source ID. These are sampling observations, **not** proof of freshest internal ECU value.
- Fixed: single selected `18DAxxF1` request ID for RPM + speed pair, or individual RPM/vehicle-speed reads when only one is demanded. One coordinator instance is shared across SmartRing and iPad. Never combine different ECUs into one pair.
- On 3 consecutive physical group errors, Recheck once with 8 new functional probes. **Original ECU only** may recover. Other winner or second failure -> Legacy individual functional Mode01 reads; no silent ECU switch.
- Failure of lease, driver, mode or bus is never authorization for more TX. Existing fault lock wins. The 8 + 8 discovery cap is per boot; CAN data is not persisted to NVS.

## Boundaries
- `DiagnosticPolicy` maintains 256 source-ID bits in atomic **volatile RAM**, initially empty. ONLY the serialized CAN owner records an ECU if the functional Mode01 grouped result contains an entire `41 0C 0D` pair with matching actual RAW Single Frame. Physical Mode01 admits only `01 0C`, `01 0D`, `01 0C 0D` directed to such observed ECU; unrelated Mode01, Mode09 and UDS are denied except pre-existing ECU01 `UDS22`.
- Physical ISO-TP reply must be `18DAF1xx` from exactly the source addressed; never flow-control a purported multi-frame 6-byte pair. RX RAW journal remains original, virtual per-PID frames are for ELM only.
- 50ms per logical Mode01 parameter is keyed identically across functional and observed physical request IDs. CAN TX Lease, single CAN owner, Listen-Only startup/between transactions, BUS-OFF lock and per-PID cache freshness are unchanged.
- Old `m5dial` firmware remains untouched. All new active behavior requires **M5CAN_ECU_SELECTION_EXPERIMENTAL** and the named `m5dial_ecu_selection_lab` environment.

## Logging / acceptance
- Serial `#ECU_STATE` prints state, chosen responder, probes, recheck count, physical successes/failures; `#ECU_CAND` lists each candidate's successes, observed changes, average reply delay and disqualification; `#ECU_TRAFFIC` includes actual CAN-owner `tx_attempts`, `tx_success`, bus RX and grouped observed RAW RX frames. Global CAN RX counts include unrelated traffic; do not pass them off as exact ECU-only wire traffic.
- Host regressions: observed-only allowlist, exact PID/service, foreign reply rejection, 50ms cross-address pacing, five responders/4rpm discrepancy, ranking, duplicate/partial, timeout, 8+8 cap, no ECU switching, two-client result routing.
- Isolated simulated-CAN ECU test before any vehicle work: actual request IDs, exactly one TX + single reply, owner lease expiry, mode transitions and no Flow Control for paired SF, BLE two-peer provenance and stack margins.
- **Owner-controlled** stationary RP8 trial only after green CI and safety review. Preserve existing partition/NVS/bonds/logs; do not upload from CI. Export actual RAW TX/RX timestamps, candidate scoring, single-ECU response success, p50/p95/p99 latency, bus-frame reduction, 0rpm/EV/accelerating transitions, and compare RPM/speed to independent reference. Do not assert faster freshness solely from different ECU payloads.
