# Signal V2-2 — 29bit CAN trial amendment (2026-10-28)

Scope: 29bit 500kbps only, five Service01 PIDs `0D/0C/05/5B/9A`. The 11bit Mode01 reply IDs 7E8–7EF are unsupported: reject, invalidate all signals, log and stop. No unsolicited boot CAN TX, UDS, Mode09, >5 PID physical implementation or modification of default legacy ELM behavior or the V2-1 CAN-disabled image.

ECU provenance: reuse #56 bounded (up to eight demand-triggered functional `18DB33F1 01 0C 0D`) discovery and #57 current-boot RAW-backed observed-ECU allowlist, and then exactly one physical `18DAxxF1` request. Saved hint is never permission for TX; this initial V2-2 build deliberately uses fresh discovery rather than NVS hint restoration. Multiple viable responders are Rejected(Ambiguous); no silent switching and no functional fallback after pin loss. Response ID `18DAF1xx` is shared by all five signals, never rewritten to an arbitrary expected ID.

Once pinned, one request `01 0D 0C 05 5B 9A` sends five PID selections (max six permitted in one Service01 single-frame request), then ISO-TP reassembles full response; each signal keeps genuine raw `41 PID values` and a monotonic acquisition timestamp. Speed first; RPM second. Missing/negative/timeout/unmatched/ambiguous/ISO-TP error/deadline expiry => Invalid, never valid zero. Snapshot and notification do not refresh freshness.

A two-Central profile union takes the minimum period per PID. Default 200ms (5Hz); explicit 10Hz opt-in permits 100ms. All CAN goes through the one CAN owner and an authenticated, time-bounded USB-arm gate. Sliding limits at 500kbps: at most 25,000 worst-case diagnostic bus bits in any 1,000ms and 5,000 bits in any 100ms. Conservatively count each extended 8-byte frame as 200 bits including stuff/interframe. Budget includes request, response and FC; under pressure shed auxiliary 05/5B/9A first and retain speed+RPM, otherwise STOP. Log all denied reservations and load snapshots. Real bus-analyzer load is independent evidence.

**Future design only — >6 PIDs:** round-robin slots `[0D,0C,A,B,C,D]`, `[0D,0C,E,F,G,H]` etc, up to four extras per request; speed/RPM appear in every slot. If K slots cycle at one-request interval T, auxiliary PIDs recur at best K*T; all per-PID maximum intervals/deadlines must be re-admitted or RELAXED/REJECT. No physical split implementation in V2-2.

The structural MaxSubscriptions=16 never overrides the complete Snapshot capacity: `20 + Σ(44 + RawLength) <= 486` for CAN (legacy DEMO prefix16). Reject overflow with reason `SNAPSHOT_TOO_LARGE=1`, malformed profile `PROFILE_INVALID=2`. No partial or fragmented Snapshot; split Snapshot is a future change.

Branch: stack on #62 rather than #57 because #57 is an independent branch from main. Reuse safety helpers from #56/#57 without enabling hint restoration. Virtual ELM/ISO-TP simulator #61 remains a separate test dependency.


## Address-width migration contract (owner decision 2026-10-28 23:50 JST)
V2-2 has a protocol-neutral `EcuRoute { CanIdWidth, physical_request_id, response_id }` describing either Standard11 (`0x7E0..0x7E7` requests/`0x7E8..0x7EF` replies) or Extended29 (`0x18DAxxF1` physical request/`0x18DAF1xx` response). The *accepted profile and current-boot ECU pin* determine both format and response identity. No 29bit numeric constant is a permanent API invariant. Current hardware trial enables only Extended29; a detected Standard11 reply is logged, all values Invalid and TX halted. A later separately reviewed PR will implement Standard11 TX. Until then 11bit must not be silently remapped to 29bit or accepted as valid source.

## Discovery and 11/29-bit source matching (2026-10-29 review update)

- `EcuRoute` is the shared *logical* source model: `CanIdWidth { Standard11, Extended29 }`, request CAN ID and response CAN ID. The accepted profile binds the logical ECU; the CAN owner binds its current-boot RAW-backed physical route. Both width and response ID must match, not just the numeric low bits of the identifier. This revision leaves Standard11 transmission unimplemented and rejected.
- In the V2-2 29bit trial image, a Standard11 Mode01 responder in `0x7E8..0x7EF` triggers `11BIT_OBD_RESPONSE`, invalidates all five signal qualities and revokes CAN TX. No implicit protocol fallback, no substituted 29bit header, no cached value promoted as LIVE. A later separate PR must add Standard11 discovery, CAN-owner send/FC/address binding and host/vehicle safety tests.
- The V2 profile `ResponseCANID=0` means the adapter's **currently verified pinned ECU**, not a request for any source. After selecting a source, V2 DATA/SNAPSHOT includes the genuine response CAN ID, width inferred only through the strict `EcuRoute` mapping. If an established ECU changes, the entire affected batch is invalid.
- For Safe SCAN on SmartRing, V2 opt-in images advertise the Signal V2 Service UUID `f9c17d00-2a3e-4e18-89a2-d7c1b539e000` as their one 128-bit advertising service identifier. The legacy V1/default image continues advertising its original ELM service; V1 runtime stays independent. SmartRing must never classify a device by name or old V1 ELM service as V2-compatible. It subsequently verifies encrypted GATT, HELLO authorization, profile and first Snapshot prior to storing the target.

## Independent review corrections (PR63)

Functional discovery's possible induced RX is explicitly 256 source addresses
times two SF replies, plus request = 513 frames. A full reservation is refused
before any TX under the rolling caps; the eight-entry selector is not a
bus-traffic bound. Active first-car acquisition remains NO-GO until a separate
bounded discovery design is reviewed. Physical requests reserve request,
10 candidate RX and one FC before transmission (essential pair: no FC, at most
two RX). Duplicates/overlap/capacity exhaustion latch transaction error and
clear positive results. Reservations remain charged throughout the response
interval plus each rolling window; actual candidate RX including late traffic
is counted at receive time and independently cap-checked. FC is charged before
TX. External traffic beyond the budget locks out adapter TX; this is not a
claim that the adapter can silence a faulty ECU.

Pin loss latches STOP. Only a new explicit ARM authorization epoch may reset
discovery; old completions are drained and rejected before any new submission.
SourceGeneration/ModeEpoch advance on authorization changes and STOP, and
queued DATA/Snapshot must match both. Same-trial ARM renewal does not repin.
A receiver requiring constant generation 1 must be updated and independently
reviewed before physical acceptance; fail-closed rejection is not LIVE success.

## Paired Ring B2 wire coordination — REQUIRED before matched acceptance

Companion Ring PR #139 head `b01fb383fa95526b5a960361e44cc3f058dbdd39`
now requires the age/correlation extension in
[the paired wire contract](signal-v2-2-ring-age-contract.md). CAN SignalRecord
uses RecordType=2, unchanged length/layout, offset34=`AgeAtEncodeUs` instead
of uncertainty. Preserve ObservedAtUs/SampleSeq across cache/Snapshot; recompute
age on encoding each PDU. CAN Snapshot uses p[13]=2, p[14..15]=0,
p[16..19]=Snapshot Control RequestId echo, records at20. DEMO remains legacy.
Snapshot RTT supplies a receiver-side transport-age bound including delayed
FIRST/fragments/queues; later DATA maps ObservedAtUs through that bound.
SourceGeneration/ModeGeneration must both advance across STOP/ARM.

This adapter implements the CAN type2 / acquisition-age and correlated
Snapshot prefix20 extension. DEMO remains type1/prefix16. Host tests verify
the exact echo, recalculated age, queue rejection and generation changes.
The software contract still requires independent paired hardware acceptance;
it does not override the functional-discovery NO-GO gate above.

## N1 PR-A: evidence-bounded physical discovery — docs-only (2026-10-29)

See [N1 candidate/provenance, SF contract, rolling-bit admission and approval gates](signal-v2-2-bounded-ecu-discovery-pr-a.md). Owner RP8 historical evidence records FIVE 29-bit responders; the **only proposed, not-yet-approved** priority candidate is `18DAF101`, chosen from the documented provisional ECU01 preference, not from guesswork. Other observed responders remain in the evidence table and are not declared false. The proposed `0C/0D` physical 2-SF cap has **no per-physical-query raw-frame proof** in the three cited documents; therefore active vehicle acquisition remains **NO-GO** until independent verification and PR-B/C. Discovery functional TX remains prohibited. Full conservative admission retains 200 bits/frame, 4 frames per candidate, 4,000/5,000 bits in a 100 ms envelope and 5,600/25,000 bits in a 1 s envelope; the upper and lower gates cannot be bypassed. STOP/ARM never reuses a prior pin.
