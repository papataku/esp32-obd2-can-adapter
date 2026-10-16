# Issue #31: RPM observation evidence — Phase A (RAM core)

## Scope and source of truth

This stage follows [adapter issue #31](https://github.com/papataku/esp32-obd2-can-adapter/issues/31) on top of main ee3d6c2. It is **not a complete recorder or a hardware release**.

Added \`src/rpm_observation_log.h\`: an allocation-free, single-owner 64-run RAM compressor (compile-time budget <=24 KiB), and deterministic host test. No CAN/BLE code calls it yet. No flash format, partition, TX policy, cache/pacing, device behavior or USB commands are changed.

## Acquisition versus delivery

- Each \`RpmObservation\` is one **adapter-side observation**. Its timestamps are not ECU internal sample times.
- \`observed_us\` is monotonic adapter observation time; \`acquired_us\` is the original CAN completion when known. Cache delivery must retain the original \`acquired_us\`; zero means unknown.
- \`acquisition_id\` links deliveries to an original result. Never create a guessed ID; zero means unknown.
- \`origin\` distinguishes fresh CAN, CAN-owner cache, coordinator cache, synthetic, and unknown. \`physical_tx\` must be set only after actual TWAI submission succeeded; merely requesting data is not a physical TX.
- \`grouped\` identifies an actual physical 01 0C 0D CAN request, **not** the protocol's BLE M5CAN batch.
- The normalizer sorts up to eight responder CAN IDs; it rejects duplicate CAN IDs. It does not fabricate extra ECU replies. Zero RPM is a valid positive 41 0C result, whereas NoData/timeout/negative outcomes are separate.
- Compression keys on request, grouped mode, status, and complete normalized ECU responder/value/validity set. The source category may vary without splitting the run; source-specific counts are preserved.
- \`physical_tx_count\` only increments for reported successful fresh transactions with a known acquisition ID. It cannot prove exactly-once globally; a real hook must submit one physical completion per transaction. Unknown IDs remain unknown rather than guessed.
- Run-level first/last peer and generation, plus \`peer_changed\`, indicate when compression lost the exact per-delivery mapping. This Phase A structure is deliberately lossy: individual gaps and peer sequences must eventually be captured by an optional detail ring.
- Rejected records consume observation sequence numbers, set a gap and cannot silently merge two runs. Ring overwrite, clock regressions and invalid samples are counted.

## Verification carried out

Host sanitizer test cases cover a nine-reply 470 rpm interval (950ms), next 0 rpm after 1,052ms; single vs repeated 500->0; NoData vs 0; freshness/source mixing without double-counting CAN TX; responder ordering/conflict/missing/duplicate; rejected sample gap; ring overwrite and clock regression.

This is a synthetic reconstruction of issue #31's numerical example. **The entire original 41-minute SD session was not loaded or validated.** The live firmware is unchanged until the instrumentation and persistence phases pass gates.

## Follow-up gates (NOT IMPLEMENTED)

1. **Hook instrumentation:** capture physical CAN owner submit/completion, the true timestamp, original RPM RAW and all bounded responder results, owner-cache and coordinator-cache use, physical 01 0C 0D versus BLE batch, and peer/generation. No new polling, flash/Serial/BLE work in TWAI, callbacks or foreground-critical paths.
2. **Persistent evidence:** design a slot/index extension for \`EvidenceStore\`, keeping existing 20 protected keys, original NVS, BLELOG, DRIVELOG, firstProbe/firstFailure, ownership metadata, CRC, commit-last and recovery rules. Test capacity before adopting the proposed 256 blocks. A RAM collection ring is **not durable**.
3. **USB recovery:** versioned \`RPMLOG STATUS/INDEX/READ\` with chunk offsets, generation, CRC, interrupted-read recovery, old firmware handling, no implicit erase, and collection only during safely idle periods. Provide CSV/JSON analysis.
4. **Performance/safety:** compare logging enabled/disabled with identical dual-meter and CAN workloads, p50/p95/p99 latency, PID Hz, drops, stack/heap and flash pause maxima. Confirm TWAI mode, TX lease, read-only allowlist, BUS-OFF and pacing unchanged.
5. **Physical gate:** real M5Dial power-cut and recovery, then isolated CAN bench, then RP8 vehicle acceptance; separate synthetic/DEMO from actual CAN evidence.

**Never claim Issue #31 accepted or merge into production from this Phase A test alone.**
