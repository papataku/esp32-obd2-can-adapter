# PID sample cache / CAN pending-request planner (Issue #18)

## Invariants: separate concerns

- **Cache** (`diagnostic_pid_cache.h`): keyed by exact Mode01 request CAN address/service/PID, stores each independently validated parameter and responder, not a grouped request. Maximum age **100 ms** from completed acquisition; failures invalidate only the affected PID. Fixed 12 entries; no allocation or locks because the dual-BLE coordinator on CPU1 is the sole owner.
- **Planner** (`diagnostic_batch_planner.h`): pure function that takes *only already pending, individually unresolved requests* and selects Single / JoinSameParameter / GroupDistinct. It does not see cache, BLE, time or a queue. It never invents any PID and does not extend a shorter request timeout.
- **BLE session coordinator** (`DualBleElm` on CPU1): tracks two pending sessions with generation-aware routing, checks the per-PID cache before invoking the planner, and reconstructs the existing ordered `M5ITEM/M5DONE` envelopes. Identical pending commands join one physical transaction. Independent simultaneous RPM and speed requests from different peers can be grouped, provided the exact known opt-in ECU profile allows them.
- **CAN transaction owner** (`CanMonitor` CPU0): at most one physical query, finite TX lease, exact read-only allowlist, 50 ms per-PID pacing, TWAI state handling, ISO-TP and physical RAW capture. Cache hit is *not* permission to ignore a fault lock.
- **ECU capability** (`AdaptiveMultiPid`): independent of the cache; complete response from the learned single ECU -> grouped mode, invalid/negative/partial/timeout -> disable exact group until reboot and retry missing individual values, bus/lease/driver fault -> no additional physical TX.
- **Physical vs virtual evidence**: grouped samples used for ELM compatibility are virtual per-PID SF records; original observed CAN frames are exclusively in RAW capture/DriveLog, and must not be replaced by split virtual frames.

The stable `m5dial`, stable 20-byte dual and MTU247 profiles are **not enabled** for coordinator optimizations. New behavior is confined to `m5dial_dual_ble_multipid_bench` while correctness is established.

## Lifecycle example

1. SmartRing requests RPM; JC3636 requests RPM + speed. Each PID is recorded as a separate logical intent.
2. Coordinator looks up each exact PID in the bounded cache. Fresh values immediately satisfy those waiters. A cache miss does not affect requests for other PIDs.
3. If the *same* missing PID is already pending from two peers, the planner joins both waiters (does **not** require an ECU supporting multi-PID).
4. If a missing RPM and a missing speed are **both currently pending**, the planner may use one `01 0C 0D` grouped CAN transaction only in the opt-in build. With just RPM pending, it sends ordinary `01 0C` and does not await a future speed request.
5. Validate the response and its ECU provenance before caching; a complete pair populates **two** independent cache slots. A partial group containing one verified PID populates only that PID slot; the other PID remains a cache miss, with an individual fallback after the existing pacing guard.
6. After response completion, notify only the requesting peers, with their original `M5ITEM` order and connection generation. If a peer disconnects, it never receives a reply from its previous generation; a surviving peer retains its logical request.

## Test design — test boundary and oracle

| Area | Test | Acceptance |
| --- | --- | --- |
| Cache | Fresh/exactly 100ms old/clock rollback | Correct TTL boundary and max-age clamp |
| Cache | Multiple PIDs, invalid response, write request | No cross-PID overwrite, no unsafe caching |
| Cache | ECU mismatch, truncated RPM, partial compound | No poisoned cache entries; only independently complete positive data |
| Cache | More than 12 distinct keys | Bounded deterministic eviction; no heap |
| Planner | One pending PID | Ordinary single CAN request; no speculative PID |
| Planner | 2 identical PID requests | Join one physical TX even when group unsupported |
| Planner | RPM and speed pending (both orders) | Exact opt-in group `01 0C 0D` |
| Planner | Different timeouts, coolant only, group disabled | Remain independent single requests |
| Scheduler/ELM | Two peers with same PID or different PIDs | Exactly one CAN physical transaction where possible; correct per-peer order |
| Scheduler/ELM | Grouping failed (NRC, timeout, malformed, split ECU) | One probe, no re-probe, individual fallback; cache only safe partial |
| Scheduler/ELM | 2 peers, grouped responder changes ECU | Disable group; do not cache foreign response |
| Lifecycle | Disconnect/reconnect midflight, 250 deterministic cycles | No cross-generation delivery, no new speculative TX, no stuck queues |
| Safety | CAN fault lock, TX lease expiry, flow control guard | No cache bypass; no unsafe repeated TX |
| PlatformIO | Normal/20B/MTU247/dry/grouped profiles | Compile for all; stable profile behavior unchanged |
| RTOS | Compiler stack frames and runtime high-water | BLE core1 and CAN core0 remain isolated, no stack violation |

### Remaining gates before vehicle qualification

The current host ECU backend exercises real production ELM parser, BLE session scheduler and ISO-TP parser but **does not use actual TWAI hardware**. Thus it cannot prove electrical request timing, bus termination, real ECU support, physical TX count, or actual meter Hz under traffic. No vehicle testing or default enablement until host/PlatformIO/stack regression success and the existing physical CAN simulated-ECU acceptance procedure passes. Actual one-hour hardware soak and cold-power NVS persistence remain separate from this code-only PR.

### Future phase, intentionally not disguised as complete

1. Route USB through the same shared coordinator instead of the existing separate serialized USB request path.
2. Replace the legacy entire-result CAN-owner cache for UDS/long responses with a per-DID decoder only after record-length rules are verified; do not mutate unrestricted UDS or guess variable boundaries.
3. Extend waiter structures beyond two BLE peers with explicit bounded ownership/deadlines when a third client is needed. The current fixed-size two-session coordination has no queue window or extra latency.

These are explicit subsequent stages, not requirements silently implemented by speculative transactions.
