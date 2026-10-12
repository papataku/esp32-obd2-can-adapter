# Architecture Decision Log

This file records decisions that future changes should not silently undo.

## D001 - Repository scope

**Decision:** The repository is named `esp32-obd2-can-adapter`, while the initial firmware/product working name is **M5CAN**.

**Reason:** The first hardware is M5Dial, but the architecture should not be permanently tied to one enclosure/board.

## D002 - M5Dial + U179 is the initial hardware baseline

**Decision:** Initial bring-up uses M5Dial (ESP32-S3) and M5Stack Mini CAN Unit U179.

Initial pins:

- GPIO13 -> CAN TX
- GPIO15 <- CAN RX

## D003 - Vehicle power comes from switched USB, not OBD Pin 16

**Decision:** Normal vehicle configuration uses ACC/IG-switched USB 5V.

- OBD Pin 16 is not used.
- U179 HPWR+ is not used.
- U179 is powered from the M5Dial side.

**Reason:** Deterministic power-off and no dependency on an adapter sleep mode while parked.

## D004 - U179 termination must not be ignored

**Decision:** U179's onboard 120-ohm termination is treated as a hardware integration concern.

**Reason:** A third termination on an already terminated CAN bus can change the effective bus impedance. Vehicle topology must be measured/verified rather than assumed.

## D005 - Boot is Listen-Only

**Decision:** Firmware never boots directly into diagnostic TX mode.

**Reason:** A reboot, brownout, software crash, or saved configuration must not cause unexpected vehicle traffic.

## D006 - Diagnostic TX uses a Lease

**Decision:** TX permission is time-limited and must be checked at the transmit boundary.

**Reason:** Host/app failure should naturally cause TX permission to disappear.

## D007 - No unrestricted raw TX in early public interfaces

**Decision:** Early phases expose only reviewed diagnostic request forms.

**Reason:** “OBD-only” commands can still become arbitrary diagnostic writers if payloads are insufficiently validated.

## D008 - RAW CAN is the source of truth

**Decision:** Higher-level parsing never replaces raw evidence.

**Reason:** Required for debugging ISO-TP, ELM compatibility, timing, and signal discovery.

## D009 - Full continuous logs live on the Mac

**Decision:** ESP32 does not continuously store full-bus RAW CAN in flash.

**Reason:** bandwidth, flash endurance, capacity, and better reproducibility on the host.

ESP32 may keep bounded RAM history and selected fault captures.

## D010 - Native USB protocol is preferred for our own analyzer

**Decision:** Honda Analyzer should use M5CAN Native protocol when talking to M5CAN.

ELM327 compatibility remains available for interoperability and Golden comparison.

## D011 - KW905 remains a Golden Reference

**Decision:** Keep KW905 for controlled compatibility/performance tests.

**Reason:** It provides a known external reference while M5CAN evolves.

## D012 - Build capability in narrow phases

**Decision:** Sequence is approximately:

```text
Listen-Only
 -> Native stream
 -> leased read-only TX
 -> ISO-TP
 -> read-only UDS
 -> ELM compatibility
 -> optimization
```

**Reason:** Each layer can be validated independently and failures remain attributable.

## D013 - Initial diagnostic baseline is 11-bit / 500 kbit/s

**Decision:** Basic OBD validation is performed before Honda-specific 29-bit/UDS expansion.

**Reason:** Avoid mixing addressing/protocol complexity into basic CAN/TX safety bring-up.

## D014 - High-rate meter signals should prefer periodic CAN when validated

**Decision:** Do not assume the final meter must poll every displayed value through OBD/UDS.

**Reason:** Periodic CAN can provide higher update rates and allows multiple signals to be observed concurrently.

OBD/UDS remains appropriate for values that are not available as validated periodic CAN signals.

## D015 - Prior prototype work is input, not unquestioned source code

**Decision:** Earlier Phase 1-4A.1 prototypes created during design discussions may be reused, but must be reviewed and revalidated before entering this repository.

Known review lessons are listed in `AGENTS.md`.

**Reason:** This repository is now the durable source of truth.

## D016 - Per-parameter diagnostic pacing under a shared CAN arbitration ID

**Decision:** The 50ms minimum spacing applies only to an identical
read-only diagnostic request key: (CAN arbitration ID, payload length,
service, PID/DID). Different Mode01 PIDs such as RPM `010C`, speed
`010D`, HV voltage/current/power `019A`, coolant `0105`, SOC
`015B` are independent despite all using the functional request ID
`0x18DB33F1`. UDS `22xxxx` DIDs are independent as well.

**Reason:** The former arbitration-ID-wide throttle inserted an
unnecessary 50ms delay between *different* fields in a serialized
SmartRing-to-M5Dial M5CAN batch. An individual PID's repeat request
must still be throttled; the whole batch is *not* forced to 20 items/s.

**Safety:** The CAN owner task still runs one ECU transaction at a time
and matches the request/ISO-TP response, including necessary Flow
Control. Whitelisted read-only requests, finite TX leases, CAN fault
handling, response quiet time and physical bus modes are unchanged.
The pacer uses a fixed 32-entry bounded cache and never evicts an
unexpired key; if full, it delays for an expired slot rather than
bypassing a live rate limit.

**Scope:** M5Dial adapter firmware only. SmartRing's per-PID Hz
configuration and DEMO/LIVE mode selection are unchanged.

**Validation:** Host tests assert same-arbitration-ID different-PID
no-delay behavior, same-key 50ms spacing, different UDS DIDs, rejected
CAN IDs, and cache saturation. CI + PlatformIO build followed by
controlled CAN bench metrics are required before any vehicle
performance validation. An actual 60Hz per PID is not supported by
a retained same-PID 50ms limit.

## D017 - Dual-meter stability baseline retains 20-byte notifications

SmartRing and JC3636W518C M5CAN bench clients have a bounded 20-byte
notification receiver unless the MTU experiment is explicitly enabled.
Negotiating ATT MTU247 does not enlarge this application receiver. The
dual adapter must therefore cap its targeted notification chunks at 20
bytes for the current stability baseline. Larger chunks reproduced RX
queue rejection, batch timeout and repeated reconnection with SmartRing
alone. This cap applies only to the experimental dual environment;
production transport and CAN safety policy are unchanged. Throughput
experiments must coordinate sender and both receiver profiles.

The regression test uses two distinct connection handles and large MTUs,
checks a 203-byte reply arrives as individually targeted chunks <=20 bytes,
and verifies disconnect/reconnect generation isolation. Physical USB-only
dual-client runtime evidence is recorded separately.

### Matched MTU247 throughput bench

`m5dial_dual_ble_mtu_bench` explicitly raises the notification cap to 244
bytes. Use it only with both meter receivers built with
`VEHICLE_M5CAN_MTU247_EXPERIMENTAL`; it is not compatible with the old
20-byte receiver. The default dual bench retains its 20-byte cap. Peer
MTU still bounds each send, and unicast, generation checks and bounded
backpressure behavior are shared. Host tests run both caps and compare
payload bytes, including retries. A short performance comparison does
not replace the one-hour stability acceptance for either profile.

## D018 - CAN owner scratch is fixed storage, not nested stack frames

On the USB bench, selecting DEMO OFF reproduced a stack-canary panic in
m5can-owner during `twai_driver_uninstall` / `esp_intr_free` cross-core IPC.
The compiler reported rxTask 3392 bytes plus performQuery 3040 bytes,
leaving too little of the 7168-byte task stack for driver/RTOS calls.
Move the bounded result and transaction into CanMonitor storage owned
exclusively by its one CAN task. Return results through an output reference,
with queue/cache copies completed before scratch reuse. Reset the transaction
on TX-error paths as well, so prior replies cannot become the new result.

CI rejects either owner function growing beyond 512 bytes. The optional
canmode dry bench exercises Listen-Only/Normal/Listen-Only mode transitions
with `transmitOwnedFrame` returning before any physical TX call. This checks
the reproduced stack path on disconnected USB hardware without CAN traffic.
It does not establish live-ECU transaction/caching acceptance. Diagnostic
allowlist, finite lease, single owner, cache and fault rules are unchanged.
## D019 - Experimental probe-once multi-PID CAN request with failback

**Decision:** An opt-in dual-BLE firmware may test only the known read-only
Mode 01 pair `010C,010D` as one `01 0C 0D` CAN transaction. The first
eligible real-CAN batch probes the pair once. A complete matching positive
reply from one ECU enables grouping for the remainder of the boot. Any
negative, incomplete, malformed, timed-out or ISO-TP-failed reply disables
grouping and falls back to the previous individual PID path for the rest of
the boot. Driver/lease/BUS-OFF faults never trigger speculative fallback TX.

**Preserved invariants:** Normal builds retain existing request forms; the
same-PID 50ms pacing is enforced for *each* constituent of a grouped query.
Read-only allowlists, lease checks, query ownership, response affiliation,
bounded ISO-TP, CAN fault locking, per-connection BLE isolation and RAW CAN
logging remain in force. Logical split ELM replies must never be interpreted
as actual RAW CAN frame evidence.

**Follow-up:** Only expand beyond RPM+speed after controlled ECU replay/bench
results. Multiple UDS DIDs require separately verified record boundaries.
See [adaptive_multipid_pilot.md](adaptive_multipid_pilot.md).

## D020 - Grouped CAN pilot requires a separate build opt-in

The PR15 host scheduler tests and USB DEMO checks cannot qualify physical CAN
behavior. Keep the exact grouped read allowlist behind
`M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL` in the separate
`m5dial_dual_ble_multipid_bench` profile. Existing dual-client stable profiles
continue individual queries. Merge the tested implementation without advancing
the physical CAN or vehicle acceptance gate; those remain pending.

## D021 - Retain bounded post-drive summaries instead of full CAN flash logs

The vehicle test may run without a USB host. Store eight boot summaries and the
last eight diagnostic state events per boot in NVS, checkpointed by the Arduino
loop. BLE only touches protected RAM. Retain original sample RX frames for group
success/fallback/fault decisions, counters and reset/build metadata. USB DRIVELOG
retrieves history without clearing it. Continuous RAW CAN still belongs on the
host. Pending data can be lost on power cut between checkpoints; report this
limit and storage failures explicitly. See `docs/drive_log.md`.
