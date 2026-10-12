# Integrated logger and two-meter acceptance

Integration includes adapter PR11,22,25,26,27,28 and meter PR82,85 on their
current main branches. iPad hardware/app integration is explicitly excluded.
Default CAN policy and all experimental group/MTU feature gates stay intact.
Actual vehicle multi-ECU and ISO-TP acceptance remain in Issue23/24, not inferred
from DEMO or simulated data. SmartRing and JC3636W518C are the meter targets.

## Storage correction

Read-only physical backup found NVS20KiB and a completely erased1.5MiB spiffs
partition at0x670000. No partition table/configuration erase is permitted.
Legacy trip/coordinator/configuration stay in NVS. CAN first probe/first later
failure (8 boot slots each) and BLE evidence (4 boot slots) now use a dedicated
raw journal in that existing unmounted partition. It must not be mounted by a
filesystem. Every byte must be erased before first ownership initialization;
foreign data fails closed without formatting. Existing owner metadata is
redundant; no new partition is created and no user filesystem is overwritten.

Each blob is appended to a4096-byte sector with monotonic generation, type,
slot, length and CRC32; commit marker is programmed last, then read back.
The latest valid record for EACH of20 keys stays protected until replacement
commits.382 data sectors distribute erases. Torn erase/header/payload/commit
and corrupted records recover an earlier valid committed value at restart.
Initialization torn before its first owner commit fails closed and requires
operator recovery from backup; no automatic erase of unknown contents.
Storage is bounded event capture, not continuous RAW CAN flash recording.

BLE schema3 retains the entire40-char SHA plus-dirty within48 bytes; legacy
schema2 NVS records are CRC checked and converted READ ONLY for export. Legacy
DriveLog v2 ABI and full-RAW sidecar v1 ABI stay unchanged. CRC failures,
unsaved state, dropped diagnostics, sequence gaps, eviction, unknown codes,
and storage failure counters are visible. Full SPIFFS/NVS backups stay private.

## Coverage correction

Transport status9 non-OK codes, raw NimBLE errors, disconnect/reject, queue
overflow, stale generation, busy/invalid/oversize ELM, task/init faults and
CAN health deltas remain observed. Add request identity (length,SID,PID/DID),
response ECU, grouped rejection reason, NRC count and first/last contexts at
CAN-owner observation times, including USB requests. NRC is distinct from
transport failure. New code915 covers DriveLog persistence failures as well
as code901 for BLE journal storage failure. Counts from different observation
sources must not be summed as unique incidents. The48 fault-code buckets and
12 detail events are bounded; not every request/response is retained.
Request context detail: bit31=DEMO, bits30..24=length, then SID, byte1,byte2.
CAN NRC detail: service bits31..24,NRC bits23..16,ECU low16; full response ECU
is a separate context event. Snapshot counter deltas retain occurrence counts;
some health observation timestamps have the5s sampling granularity.

Live per-event/group transition USB output is deferred during acquisition;
retrieve BLELOG/DRIVELOG with peers stopped. Generic dual snapshots expose
connect/disconnect counters so a short reconnect is not hidden by5s sampling.
The single-peer Bluedroid diagnostic HUD and opt-in MTU A/B are preserved,
without replacing two independent NimBLE sessions. Normal notify size stays20.
Meter diagnostic and bench rate commands now share one bounded @ parser.

## Software evidence (before physical test)

Host/sanitizer regressions include coherent5-ECU replies, deterministic pinning,
conflicts/malformed replies/fallback, cache freshness and peer isolation.
New flash tests cover20 keys, >800 rotating writes, CRC, every transaction cut,
foreign data/read-only inspection and redundant owner recovery. Combined64
boots retain all48 CAN frames+RX offsets, first failure and BLE reasons while
legacy settings/trips stay intact. Full SHA and read-only v2 export are tested.
LOGTEST exists ONLY in the TX-disabled DEMO fault fixture and queues explicitly
SYNTHETIC full48-frame sidecars. It cannot qualify real ECU replies; exports
mark origin=SYNTHETIC. It requires no BLE peers and verified DEMO.

Long-run profile m5dial_integrated_soak retains the grouped coordinator and
logger code with the physical CAN TX API compiled out, without fixture replies.
It does not qualify real-CAN acquisition or electrical behavior.

Physical full-RAW synthetic save/readback/warm reset and dual-meter smoke
passed at544268b. The2h sustained traffic/heap/fps/error check is running.
Physical cold power loss and human LCD/touch are still unobserved; keep these
limits explicit in residual issues. Existing vehicle issues stay open.


## Pre-soak physical blocker and stack correction

The first full integration fixture3f00008 was application-only flashed with
hash verification; its USB and BLE runtime did not respond, and esptool
could not subsequently enter ROM by default or explicitUSB reset. The2h
soak had NOT started at that point. A user power cycle was requested, then
revoked after recovery by serial-selected1MHz JTAG reset. This was a warm
reset, not a physical power cycle. Complete8MiB/16MiB backups are available.

Compiler inspection found startup frames752+2464+3872=7088 bytes before
NVS/library calls, and export could nest3632+3872 before caller frames.
The initial checker monitored only BLE-worker paths and missed these chains.
This is a concrete stack risk, not yet a confirmed electrical/boot cause.
Move trip/probe/readback/export copies to fixed per-object RAM, reuse them
only in the single Arduino owner, reset large records without temporaries,
and query boot sequence without returning a full record. New measured
frames: setup48,DriveLog begin112/save112/dump256,BLE begin256; estimated
startup/export with2048-byte library reserve2352/5328 of8192. Add these
paths to CI and recognize the integrated soak's actual dual worker rather
than selecting by environment name. Dumps now refuse active peers/queries;
DEMO verification uses non-blockingSTATS instead of a journal dump.

Host/sanitizer tests and corrected profile builds/stack check pass locally.
Meter integration's14 CI jobs pass at1860414; native evidence is verified
in persistent runner storage, remote artifact quota remainsIssue94.
Both meter images72f7ad9 are flashed and their runtime ELF hashes match
the archived binaries. The recovered adapter544268b connects both actual
peers. No merge until long-run evidence is assessed. Preserve
this failed prototype's logs (including zero-byte UART captures) and avoid
NVS/partition erase as a recovery shortcut.


## Initial physical evidence at544268b

The TX-disabled fault fixture saved boot44 full-RAW firstProbe/firstFailure
with48 frames each and RX offsets100+i*137; exports explicitly say
origin=SYNTHETIC. BLE journal schema3 stored both connect/disconnect reasons,
full40-character SHA and no storage/queue/sequence errors. After an esptool
warm reset, BLELOG44 and both48-frame sidecars were still readable from
flash; durable ready=1,foreign=0,errors=0,corrupt=0,uncommitted=0.

The TX-disabled integrated soak then started2026-10-13 00:44 JST, with
SmartRing andJC confirmed connected before the measured7200s window.
Initial opening resets and intentional export disconnects are outside the
window. mac_events.jsonl retains host UTC and monotonic receipt times. The
offline two-meter analyzer checks counter regressions, brief reconnects,
per-signal rates/ages, transport traffic, flash-save outcomes, heap/stack and
UI/DMA distributions; malformed/interleaved USB rows are excluded and counted.
No assumption equates this DEMO traffic with vehicle CAN wait/electrical
load or the original three-client iPad exploration.

Private evidence root: Developer/TacoTacoMeter-artifacts/20261012-integration.
Full flash backups and firmware binaries are not uploaded as public artifacts.
