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

Checkpoint writes reserve the same in-flight slot as diagnostic submissions,
without holding a spinlock through flash IO. Defer a checkpoint while a query
is active and reject/retry new submissions until it finishes. This avoids
inserting an observed USB flash-write stall into a CAN response window. SDK
reset=unknown on USB reset is supplemented by the raw ROM reason (schema 2).


## D022 - Observe the deployed NimBLE peer path for disconnect forensics

Issue21's original Bluedroid/global-session investigation predates the accepted
dual-peer build. Diagnostic comparisons must keep the actual NimBLE20-byte
unicast/generation isolation/CAN policy. Use a bounded raw GAP event observer
for reasons (onDisconnect lacks the reason in NimBLE1.4) and main-loop output.
Do not tear down protocol state in a diagnostics callback or silently map host
reason integers to8-bit HCI codes. Preserve raw namespace/sequence/drop data.
USB startup resets and intended client disconnects are separate from failures.
See docs/issue21_tri_device/README.md; physical fixes require separate evidence.

## D023 - Fresh-erased V2 CAN journal is a lab-only single-app layout

For the M5Dial V2 RAW/SD validation requested on 2026-10-19, the owner
explicitly allowed **deleting existing M5Dial internal Flash data**.
The physically specified ESP32-S3FN8 has 8 MiB Flash. The opt-in
`m5dial_v2_fresh_flash_lab` uses one 2 MiB factory app with
`canlog` subtype 0x40 at 0x210000 (0x460000 = 4.375 MiB), leaving
SPIFFS address space, NVS and coredump mapped but erased by a deliberate
whole-chip reset. Do not try to reinterpret an existing OTA image as
`canlog` by flashing only a new partition table.

Neither the normal `m5dial` firmware, other device Flash, T4-S3 SD, nor
iPad SQLite is authorized for deletion. A separate, explicit local
acknowledgement and exact bonded peer allowlist are required even for
a lab build. Compilation NEVER causes erasure or flashing. Flash size,
partition table, firmware hash and port must be inspected before a
manual whole-chip erase and factory image upload.

This is **single-session** append-only journal storage: two owner
sectors plus bounded committed data blocks. There is no automatic
session reclaim, log overwrite, partition migration or OTA fallback.
After testing, save/verify the actual USB/SD raw data before any new
destructive reset. M5S2 V2 CRC, durable SD ACK, legacy ELM fallback and
read-only CAN/TX lease constraints remain unchanged.

The earlier decision D021 remains valid for normal firmware: sustained
full-bus logging on Flash is not production-qualified. This decision
only enables an intentional finite V2 lab capture for obtaining evidence.
See `docs/m5dial_v2_fresh_flash_lab_2026-10-19.md`.

### Paired MTU247 ASCII path within the V2 lab

`m5dial_v2_fresh_flash_mtu247_lab` retains the V2 lab journal, bonded
single-peer allowlist, exact flash layout, sender fairness and SD durable
ACK rules, but uses the already-tested 244-byte ELM notification cap.
Both meter receivers MUST explicitly enable
`VEHICLE_M5CAN_MTU247_EXPERIMENTAL`. Negotiated MTU alone is insufficient.
The ordinary V2 lab and production profiles retain the 20-byte baseline.
This isolates ASCII fragmentation from binary V2 performance; it does not
claim to fix RF connection-establishment failure or physical CAN pacing.

On an already-verified migrated device, switch only the application after
checking the installed partition table; do not erase NVS/CANLOG, write
partitions, or perform the original fresh-install migration again. The
build acknowledgement and explicit real approved peer identity remain
required. CI tests missing-ack denial and compiles with test-only identity,
then removes that non-deployable binary.

## D024 - Two-peer V2 bulk may not preload the next PID response's host FIFO

The 2026-10-21 USB-only A2 bench reproduced a T4 RPM drop from 9.998Hz to
6.637Hz over 52.430 counter seconds while Ring remained 9.841Hz. Both links
stayed connected, queues reported no drop/invalid/full, SD made progress and
T4's last128 batch RTT p95 reached290ms. The application ELM queues being
empty is insufficient: up to eight already-accepted bulk notifications can
remain in the same peer's controller/host FIFO ahead of a future PID reply.
This is a concrete resource-ownership issue; its precise hardware contribution
still requires the paired bench after this change.

After the ordinary live NimBLE credit snapshot passes, V2 additionally admits
bulk with TWO connected peers only when that target peer has no controller
outstanding or host-queued packets. This bounds bulk occupancy to one pending
notification. ASCII response credit is unchanged, and the existing single-peer
preloaded path stays unchanged. Credit denial is transient backpressure,
counted as v2_wait_host; it must not revoke the authenticated session, reset a
cursor, erase data, or bypass durable SD ACK. No CAN, lease, journal layout,
partition, wire format, deadline or notification-size policy is changed.

FIFO/peer-transition host regression distinguishes old eight-packet head-of-line
blocking from the new one-packet bound. That model is not an RF latency proof.
Physical two-peer full-file CRC/END/SD-vs-USB and RPM-before/during/after gates
remain required; preserve the failed A2 .part for comparison.

## 2026-10-29 — PR65 bounded discovery contract enforcement

The V2 physical probe now observes the full <=120 ms response window, accepts either order of the two permitted SF PID replies, and rejects unexpected diagnostic source/format before the generic ISO-TP filter can discard it. A probe observation guard remains active during idle/pacing after completion or timeout; late diagnostic replies are charged and latch the monitor fault until recovery. Before reinstalling the driver, stop TWAI and drain its bounded RX queue so a queued query cannot erase late-response evidence. A subsequently admitted regular transaction takes ownership of its response context. Both owner scheduling and actual monitor TX admission enforce >=200 ms spacing around a probe, independently of the regular 10 Hz sample period.

The owner checks loss/fault counters and the absolute discovery deadline before promoting a queued completion. Failed lease/submission and denied discovery reservations latch STOP without retrying discovery within the epoch. The lower monitor also faults on RX queue loss, nonzero TWAI RX loss/error counters or unavailable status, checking before driver reinstall can reset counters. V2 policy refuses functional discovery, including empty-profile builds. None of these software fixes establishes the physical two-SF ceiling or advances HIL/vehicle gates in the PR-A specification.


### 2026-10-29 — R1 regular acquisition auxiliary shedding

The no-fallback discovery rule forbids functional broadcast, retries, repinning
and new candidates within an epoch; it does not forbid regular auxiliary
shedding. After pinning, prefer the full five-PID group (12 frames), otherwise
reserve the four-frame SF-only 0C/0D request to the same ECU. STOP with
LOAD_BUDGET_ESSENTIAL if that entire reservation also fails. At 100ms cadence,
full-group selection additionally leaves four frames of headroom for the next
essential read: two consecutive 2400-bit reservations would otherwise block
even an 800-bit read at the third start. This deterministic selection avoids
that dead end; 5Hz still selects every admitted full group. Both layers keep
200 bits/frame, 5000/100ms and 25000/1s caps and 120ms reservations. Essential
results invalidate auxiliary values immediately rather than retaining stale
samples. No essential FC is allowed.

An internal request marker distinguishes regular essential reads from discovery
for monitor pacing and post-response quiet-close only. Both retain the strict
SF validation and late-response accounting guard. Discovery still observes its
full 120ms window and enforces 200ms on both sides of its TX; regular essential
reads can finish after the normal quiet period. Sustained 100ms demand tests
cover owner scheduling and production monitor admission independently. This is
host software evidence, not a response latency or physical vehicle qualification.

## D025 - Dedicated passive image disconnects controller TX from GPIO

`m5dial_signal_v2_can_29bit_passive` compiles only a USB ambient receiver.
GPIO13 holds U179 TXD recessive HIGH; install uses unused GPIO39 and removes
all controller TX routes before start. This defeats ESP32-S3 Listen-Only
error-frame generation without relying on precompiled SDK errata settings.
No diagnostic, NORMAL, ARM, lease, BLE or auto-recovery implementation is
linked. Boot/100ms register and pad checks latch stop/uninstall on a fault.
This is routing isolation, not electrical isolation against arbitrary faults.
See [evidence and limitations](qa/m5dial-passive-design-review.md) and
[owner/PM acceptance runbook](qa/m5dial-passive-runbook.md). Device acceptance
remains pending; existing active/legacy gates and default env are unchanged.
