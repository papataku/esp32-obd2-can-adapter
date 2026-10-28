# Signal V2-2 — 29bit CAN trial amendment (2026-10-28)

Scope: 29bit 500kbps only, five Service01 PIDs `0D/0C/05/5B/9A`. The 11bit Mode01 reply IDs 7E8–7EF are unsupported: reject, invalidate all signals, log and stop. No unsolicited boot CAN TX, UDS, Mode09, >5 PID physical implementation or modification of the default/V2-1 CAN-disabled images.

ECU provenance: reuse #56 bounded (up to eight demand-triggered functional `18DB33F1 01 0C 0D`) discovery and #57 current-boot RAW-backed observed-ECU allowlist, and then exactly one physical `18DAxxF1` request. Saved hint is never permission for TX; this initial V2-2 build deliberately uses fresh discovery rather than NVS hint restoration. Multiple viable responders are Rejected(Ambiguous); no silent switching and no functional fallback after pin loss. Response ID `18DAF1xx` is shared by all five signals, never rewritten to an arbitrary expected ID.

Once pinned, one request `01 0D 0C 05 5B 9A` sends five PID selections (max six permitted in one Service01 single-frame request), then ISO-TP reassembles full response; each signal keeps genuine raw `41 PID values` and a monotonic acquisition timestamp. Speed first; RPM second. Missing/negative/timeout/unmatched/ambiguous/ISO-TP error/deadline expiry => Invalid, never valid zero. Snapshot and notification do not refresh freshness.

A two-Central profile union takes the minimum period per PID. Default 200ms (5Hz); explicit 10Hz opt-in permits 100ms. All CAN goes through the one CAN owner and an authenticated, time-bounded USB-arm gate. Sliding limits at 500kbps: at most 25,000 worst-case diagnostic bus bits in any 1,000ms and 5,000 bits in any 100ms. Conservatively count each extended 8-byte frame as 200 bits including stuff/interframe. Budget includes request, response and FC; under pressure shed auxiliary 05/5B/9A first and retain speed+RPM, otherwise STOP. Log all denied reservations and load snapshots. Real bus-analyzer load is independent evidence.

**Future design only — >6 PIDs:** round-robin slots `[0D,0C,A,B,C,D]`, `[0D,0C,E,F,G,H]` etc, up to four extras per request; speed/RPM appear in every slot. If K slots cycle at one-request interval T, auxiliary PIDs recur at best K*T; all per-PID maximum intervals/deadlines must be re-admitted or RELAXED/REJECT. No physical split implementation in V2-2.

The structural MaxSubscriptions=16 never overrides the complete Snapshot capacity: `16 + Σ(44 + RawLength) <= 486`. Reject overflow with reason `SNAPSHOT_TOO_LARGE=1`, malformed profile `PROFILE_INVALID=2`. No partial or fragmented Snapshot; split Snapshot is a future change.

Branch: stack on #62 rather than #57 because #57 is an independent branch from main. Reuse safety helpers from #56/#57 without enabling hint restoration. Virtual ELM/ISO-TP simulator #61 remains a separate test dependency.


## Address-width migration contract (owner decision 2026-10-28 23:50 JST)
V2-2 has a protocol-neutral `EcuRoute { CanIdWidth, physical_request_id, response_id }` describing either Standard11 (`0x7E0..0x7E7` requests/`0x7E8..0x7EF` replies) or Extended29 (`0x18DAxxF1` physical request/`0x18DAF1xx` response). The *accepted profile and current-boot ECU pin* determine both format and response identity. No 29bit numeric constant is a permanent API invariant. Current hardware trial enables only Extended29; a detected Standard11 reply is logged, all values Invalid and TX halted. A later separately reviewed PR will implement Standard11 TX. Until then 11bit must not be silently remapped to 29bit or accepted as valid source.
