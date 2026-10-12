# M5CAN-Dial ⇄ iPad — BLE latency and high-rate acquisition review

## 2026-10-10 measured baseline (single iPad SQLite session)

Source: user-supplied session-2026-10-10T06-19-47Z.sqlite3. Never commit raw vehicle captures or BLE UUID identifiers to the repository.

| Measurement | Result |
|---|---:|
| Capture window | 1322 seconds |
| Recorded ELM commands | 7819 |
| M5CAN lease renewal ATM5TX1 | 484; median latency 57.6 ms |
| Normal OBD PID median latency 010C | 86.9 ms |
| Normal OBD PID median latency 010D | 85.3 ms |
| Normal OBD PID median latency 0105 | 60.2 ms |
| Normal OBD PID median latency 015B | 59.1 ms |
| Normal OBD PID median latency 019A | 61.9 ms |
| Raw RX callback chunks | 60178 |
| Raw RX callback byte count | 456303 (~345 B/s average over capture) |
| Raw BLE RX callback size | max 20 bytes |
| Recorded LIVE_POLL_CONFIG | target=5 req/s, unthrottled=false, every time |

The raw BLE RX byte count is **not** a radio throughput benchmark: it counts delivered application notification bytes under a request/response workload, not idle capacity or on-air data. The 20-byte notification size was imposed by the firmware implementation, not necessarily the negotiated radio ceiling.

## Findings

1. M5CAN `BleElmTransport::write` has a hard-coded 20-byte notification chunk; the host uses a prompt `>` to start the next request. The firmware formerly sent line text, CR/LF and prompt as separate calls, potentially producing many BLE notifications per small response. The source now aggregates responses until the prompt in a bounded 512-byte buffer to reduce notifications, preserving the ELM wire format. No measured improvement has been established until a new iPad capture.
2. M5CAN previously had a **global** 50-ms diagnostic spacing. This has been replaced by **50 ms per CAN request arbitration ID**; different approved IDs no longer share the timer. Per-ID request rate remains <=20/s, but transaction serialization, ECU processing and the BLE command/response loop still limit aggregate throughput.
3. M5CAN transitions TWAI Listen-Only → Normal → Listen-Only **per diagnostic request** and performs 29-bit ISO-TP transaction handling. We have not separately measured time spent in those mode switches.
4. Host iPad setting 5 req/s means the five-PID baseline loop aims for ~1 Hz per PID. Raising the target may help until serialized command RTT becomes dominant, but 20 req/s/5 PIDs is an *optimistic ceiling* of ~4 Hz per PID, not a guaranteed rate.
5. A 58ms median for ATM5TX1, an AT command that does not hit CAN, shows significant non-CAN round-trip overhead. It does not isolate radio vs OS scheduling vs parser/notification framing by itself.

## Optimization plan — staged

### A. Low-risk ELM compatibility improvements

- [x] Coalesce BLE ELM text + CR + final prompt into fewer notifications (bounded 512 bytes; drain at prompt; flush progressively for long ISO-TP output)
- [ ] Report negotiated ATT MTU and BLE connection interval, PHY, notification queue/backpressure counters if supported by the deployed ESP32 BLE stack
- [ ] Negotiate a larger MTU (request 247 on M5CAN, use the **actually negotiated** MTU minus 3, not a fixed 244-byte assumption); verify with an actual iPad peer
- [ ] Test BLE 2M PHY if iPad and ESP32-S3 firmware negotiate it; fall back to 1M
- [ ] Profile RX callback → command parsed → CAN queued → CAN TX → first response → ISO-TP complete → BLE output queued/completed → iPad receives prompt
- [ ] Compare target 5/10/20 req/s using fixed safe parked vehicle conditions and detect false negative, timeout, error counters and p50/p95 latency

**No change to the 50ms CAN safety pacing until hardware proof.**

### B. Lower RTT by changing the host/device interface

Introduce `SUBSCRIBE` / `STOP` with a bounded allowlist of already-validated, read-only PIDs/DIDs, controlled TX lease, backoff and maximum request rate in firmware. M5CAN locally schedules requests; iPad no longer issues the next request only after seeing `>`. This removes per-request iPad BLE control latency, **but cannot exceed ECU/CAN safety constraints**.

Send timestamped compact binary results, for example sequence number, CAN ID, DLC/payload, status, timestamps; batch several results per GATT notification and support loss detection/replay. Keep ELM327 compatibility for KW905.

### C. High-frequency signal feed (preferred when possible)

When a required RPM/speed/motor field is proven to exist in periodic RAW CAN frames **visible on the vehicle-side diagnostic connector** and validated against independent references, use Listen-Only TWAI RX with no repeated diagnostic TX. Stream selected CAN frames or decoded binary signals to iPad, batched over BLE, and decouple data sampling from UI frame rate.

Vehicle gateways may filter relevant proprietary CAN messages from the diagnostic connector; do not promise access before a real capture.

### D. Alternative transport

For sustained high-throughput RAW CAN capture, consider a Wi-Fi AP / UDP or TCP binary streaming mode for iPad, potentially keeping BLE for discovery/configuration. Verify Wi-Fi/BLE radio coexistence. Wi-Fi does not increase the ECU's diagnostic response speed and does not remove safe CAN pacing. USB Native is the controlled benchmark/reference, although inconvenient in the vehicle.

## Per-CAN-ID diagnostic pacing (approved 2026-10-10)

The former global `kMinDiagnosticIntervalMs=50` guard delayed **every** diagnostic request, even after switching to a different request arbitration ID. It has now been replaced by `DiagnosticRequestPacer` in `src/diagnostic_request_pacer.h`, invoked by `CanMonitor::performQuery`.

| Next CAN TX request ID | Control |
|---|---|
| Same approved CAN ID as an earlier request | Wait until 50 ms after its last successful request TX |
| Different approved CAN ID | No *cross-ID* 50 ms guard; each has its own history |
| 18DB33F1, 18DBEFF1, 18DA01F1 | Only these explicitly allowed IDs are managed |
| Flow Control frames | No added diagnostic rate guard; ISO-TP integrity gates remain |
| In-flight transaction | **Always serialized**: the CAN owner continues to finish or time out one query before beginning another |
| TX rejected, lease expiry, bus-off | Existing fail-closed protections remain |

Important: multiple request CAN IDs can still reach the **same ECU**. Distinct arbitration IDs do not guarantee distinct ECU processors or simultaneous capacity. The application must still limit aggregate ECU request load using measured p95 response time, error rate and fairness. Removing the global 50 ms guard is not permission to run parallel unrelated diagnostic transactions.

Host regression: `tests/test_diagnostic_request_pacer.cpp` asserts same-ID spacing, independent-ID immediate eligibility and unapproved-ID noninterference. Physical vehicle proof remains required before claiming a measurable gain.

## Acceptance metrics for each stage

- Measured p50/p95/p99 request-to-complete response latency and actual requests/sec (5/10/20 target).
- Ratio of CAN time, TWAI mode switching, BLE command/notification time and host scheduling.
- BLE notification sizes/count, negotiated MTU, connection params, drop/overflow/error counters.
- End-to-end payload equality (KW905 vs M5CAN) and no missing data or stale responses.
- CAN TX lease enforced, startup Listen-Only, no unapproved requests, no unrelated Flow Control.
- Stable capture with real vehicle and iPad, not just a compile success.

References: Espressif BLE throughput FAQ; Apple CoreBluetooth maximumUpdateValueLength / maximumWriteValueLength; Nordic BLE Data Throughput guide; Apple BLE accessory connection parameter guidelines.
