# V2-2 29bit first trial — owner procedure, 5Hz only

**Current entry status: NO-GO for active vehicle acquisition.** Functional
broadcast discovery cannot prove the required induced-RX bound: up to 256
29bit source addresses can each return two single-frame PID replies. Its
513-frame reservation exceeds the 5,000-bit/100ms cap, so this revision
refuses it before TX with `LOAD_BUDGET_DISCOVERY` and latches STOP. Eight
stored candidates is not a bus-load bound. Do not bypass this gate, provision
a guessed ECU, or substitute legacy firmware to continue. A separately
reviewed bounded discovery mechanism is required to unlock phases 4–6 below.
They specify the acceptance procedure for that future revision, not permission
to transmit with this revision. Passive inspection remains distinct.

Current implementation clarification (2026-11-01): #65 bounded physical discovery,
#67 S1 and #66 serial HIL are already merged into #63 (not main). The 5Hz
and 10Hz physical images both enforce `M5CAN_SIGNAL_V2_STAGE_GATED=1`:
`SIGV2 CAN ARM` is refused, and only separately authorized `SIGV2 CAN S1`
can request one physical probe. S2/S3 and ongoing five-PID acquisition remain
unavailable in these physical images. The unstaged five-PID path is exercised
only in the serial-HIL image, whose linked ELF must exclude physical TWAI.
The future-gate language below refers to operational approval and S2/S3;
it does not imply that merging the existing implementation grants approval.
For ambient-only vehicle inspection use the dedicated
[passive runbook](m5dial-passive-runbook.md), including its mandatory LCD gate.

This is an OWNER-OPERATED procedure. Agents must not flash, erase, connect to
real CAN, or operate a vehicle. First obtain independent review of exact
adapter and Ring SHAs, green host/policy/build CI, receiver freshness/epoch
acceptance, and the production-core serial-HIL evidence below. **10Hz is excluded
from the entire first trial**, including after stationary success. Its
compile-only CI image is not permission to use it. Use matched explicit
5Hz environments and record actual per-PID rates; requested rate is not proof.

## N1 PR-A (docs-only): candidate approval and the three-second gate

See [the RP8 candidate evidence and bounded physical-discovery specification](../protocol/signal-v2-2-bounded-ecu-discovery-pr-a.md). The three iOS owner-log-derived docs prove five observed 29-bit reply IDs, but **not** the induced maximum of two Single Frames per physical `0C/0D` request. The owner **approves the candidate list `{18DAF101}` on 2026-10-29**, but this is **not a TX authorization, proof of a two-SF upper bound, or current-epoch ECU pin**. All other observed sources and RPM conflicts remain unresolved. Therefore **this PR-A does not alter NO-GO:** phases 4–6 and vehicle ARM remain barred.

For a future independently approved PR-B/C implementation only: explicit stationary ARM starts a NEW 3,000 ms epoch with all values Invalid; send individually admitted physical `0C/0D` only to owner-approved evidence candidates (<=4, currently one), at least 200 ms apart, 120 ms response windows, no discovery FC or functional TX. Four frames/800 bits per candidate; retain 200-bit charges including late traffic and BOTH admission gates. Worst conservative envelopes: **4,000/5,000 bits per rolling 100 ms and 5,600/25,000 bits per rolling 1 s**, with preexisting reservations additionally enforced. Only a current-epoch RAW-verified, explicitly approved unique ECU may pin. **S1 is one probe plus 1-second count followed by automatic STOP, with values Invalid; it does not require or establish ongoing Fresh.** For S2 and S3, separately reserve the first regular physical read and require genuine Fresh speed zero within 3 s. Missing S1 self-measured frame-count evidence, S2/S3 source correctness, stage authorization, budget or timely acquisition => STOP/Invalid, no automatic retry/functional fallback and no pre-STOP pin reuse. Phases 1–3 remain passive/offline for diagnostics until explicit S1 entry approval; later S2 and S3 need their own approvals. No autonomous trial acceptance.

## 1. Inventory, wiring and protected backups (offline, no vehicle)

Entry: both boards physically identified by the owner; vehicle disconnected.
Follow [SAFETY wiring, switched USB power, non-isolated grounding and U179
termination checks](../SAFETY.md#1-power-and-wiring), including sections 2–3.
OBD pin16 and U179 HPWR+ stay unused. Measure termination only powered down;
do not add an unverified third 120-ohm terminator.

Follow the exact [V2-1 backup and installed-partition/app-only gate](signal-v2-1-bench-runbook.md#build-and-backup-gate)
and `tools/signal_v2_flash_check.py`. Rediscover each board's identity, flash
size, partition table/hash, active app slot, otadata, installed build/ATI and
app capacity. Use a DIFFERENT protected directory per board and per backup
session; never reuse filenames in a shared directory. Keep identities, keys,
bonds, NVS, VIN, locations and captures private.

Owner example for ONE board (repeat in another new directory for the other):

```sh
umask 077
# BACKUP_DIR is an owner-selected unique absolute per-board/session directory.
# PORT, FLASH_SIZE, APP_OFFSET, APP_LENGTH come from verified inventory.
mkdir -m 700 "$BACKUP_DIR"  # must fail if it already exists
cd "$BACKUP_DIR"
python3 -m esptool --chip esp32s3 --port "$PORT" flash_id
python3 -m esptool --chip esp32s3 --port "$PORT" read_flash 0 "$FLASH_SIZE" full-before.bin
python3 -m esptool --chip esp32s3 --port "$PORT" read_flash 0 "$FLASH_SIZE" full-independent-read.bin
cmp full-before.bin full-independent-read.bin
python3 -m esptool --chip esp32s3 --port "$PORT" verify_flash 0 full-before.bin
python3 -m esptool --chip esp32s3 --port "$PORT" read_flash "$APP_OFFSET" "$APP_LENGTH" original-app.bin
python3 -m esptool --chip esp32s3 --port "$PORT" verify_flash "$APP_OFFSET" original-app.bin
shasum -a 256 full-before.bin full-independent-read.bin original-app.bin > backup.sha256
```

Read failures, CRC errors, differing reads or missing on-chip verification
fail this gate. Confirm original-app bytes equal the exact slice of the full
backup; save partition/slot metadata separately. Protect an additional copy.
The **only rollback source is this owner's verified original backup image**.
The default `m5dial` is the existing request-driven, TX-capable ELM327 adapter;
it is **NOT a CAN-disabled fallback**. Never rebuild default for rollback.
V2-1 `m5dial_signal_v2_can_disabled_lab` excludes TWAI init/TX at link time.
Only the two V2-2 environments enable the **Signal V2** TX path. Other legacy
profiles retain their separate existing ELM behavior.

For M5Dial this gate accepts only the installed 8MiB factory layout with
app offset **0x10000** and capacity **0x200000**. Run the checker against the
verified backup and the exact new binary; require its `app.offset=65536`,
`app.size=2097152` and `write=APP_ONLY` before setting `APP_OFFSET=0x10000`.
A different layout is a STOP, not a reason to migrate. SmartRing offsets and
capacity must be independently checked by its owner; never copy M5Dial values.

This V2-2 procedure overrides the older V2-1 full-bank restoration paragraph:
keep the full backup for recovery, but routine rollback here restores only the
verified original app at the unchanged active slot. A full-bank restore needs
a separate reviewed recovery plan. Use the V2-1 preferred ROM recovery only
if required by the owner's verified tool/device procedure; never combine
ROM/esptool and JTAG operations blindly.

After review, exact layout/active-slot comparison and capacity check, owner
app-only write (no generic upload, erase, partition or bootloader write):

```sh
test "$(wc -c < "$FIRMWARE_BIN")" -le "$APP_LENGTH" || exit 1
python3 -m esptool --chip esp32s3 --port "$PORT" write_flash "$APP_OFFSET" "$FIRMWARE_BIN"
python3 -m esptool --chip esp32s3 --port "$PORT" verify_flash "$APP_OFFSET" "$FIRMWARE_BIN"
```

Before vehicle attachment, demonstrate original-app restoration offline at
the unchanged verified active slot, verify_flash, independently read back and
compare the whole original app, reboot and confirm original behavior/build.
Then repeat approved trial app-only installation and verification if proceeding.
If slot/otadata/layout changed, stop for a separately reviewed recovery plan;
never guess offsets or perform automatic full-flash restoration.
Exit: per-board verified restore evidence, backups, target SHA/env/flags,
firmware SHA256, app capacity, partition and V2-1 linked-symbol exclusion evidence.

## 2. Required on-device serial HIL (before any vehicle ARM; no analyzer)

Use an actual M5Dial with a **dedicated serial-HIL environment** preserving production CanOwner, two-level admission budget, STOP/ARM epoch, and Signal V2 wire logic. Only the TWAI driver boundary is replaced by a debug-serial PC virtual-ECU bridge. **Disconnect real CAN; TWAI must not initialize and hardware CAN TX must be unavailable** (verify env/compile/link symbols and runtime log). This is not the physical CAN output image.

PC injects 0/1/2/3 SF, FF/CF, duplicates, wrong ID, 11bit, timeouts, late frames, TX fails/retries, overflow, STOP/re-ARM races and source-generation/Snapshot replays. Repeat the 60-second cases and admission boundary tests under real M5Dial task scheduling. Require zero discovery FC, full four-frame 800-bit reservation, correct retained 100ms/1s budgets, immediate STOP/Invalid, stale-result exclusion, and the 3,000ms acquisition deadline. Theoretical 160-bit max per 29-bit 8-byte frame with stuffing/intermission; retain 200-bit/frame accounting. Record relevant real-time counters and script versions; no on-wire stuffing trace is required.

The serial bridge can validate code, budget and timing, **not a vehicle ECU's induced-response count or actual bus electrical load**. Proceed to a single S1 request only after review and explicit stage authorization. No hardware programming or vehicle action is instructed to an agent.

## 3. Passive parked vehicle inspection — never ARM here (2 minutes)

Entry: phases 1–2 passed for the intended scope; owner verified wiring,
power/termination and healthy vehicle baseline. Park, P, parking brake, IG ON;
no other diagnostic tester. Keep a second operator and record complete adapter self-monitoring counters.
Boot unarmed, encrypted approved Ring may connect, but do not issue ARM.

Observe passive bitrate/address format, error counters and warning lamps.
Listen-only cannot solicit ECU discovery or prove Mode01 support. An ECU
response ID seen passively is evidence to review, not a pin/authorization.
The adapter's own TX counters and authorization logs must show zero adapter request/FC TX for 2 minutes; this is self-measurement, not independent proof. Any TX, warning,
bus error, overrun, dropped monitoring frames or unverified wiring aborts.
Exit: clean passive evidence. This revision stops here for vehicle work.

## 4. S1 — owner-approved single physical request and 1-second RX observation

**Future gate only (NO-GO until PR-B/C + serial HIL + owner approval).** Park, P, brake, IG ON; secure authorized peer and exact owner-approved candidate `{18DAF101}`. After an explicit new-epoch ARM, send at most **one** individually admitted `18DA01F1` Service01 `0C/0D` request; do not send functional or FC. Charge four 8-byte frames = **800 bits** before TX at both admission layers. Count **every RX frame**, all `18DAF1xx` sources, SF/FF/CF, errors, overflow and unexpected ID from the pre-TX baseline through the following **1,000ms**; record TX and zero-FC counts, timestamps and rolling budget. Then **automatically STOP** and maintain Invalid. Complete/abort within the 3,000ms time guard; do **not** advance to ongoing 5Hz/live acquisition in S1.

If a third SF, FF/CF, 11bit, source mismatch, duplicate, late message, counter loss, RX overflow, TX/error/retry uncertainty or budget violation occurs, immediately STOP/Invalid and cancel all *future* requests/FC (an on-wire frame cannot be recalled). Retain late RX monitoring for future admission. Log is **self-measured, not externally verified**. Owner and PM review S1 evidence before granting separate S2 approval. One second of clean traffic is not proof of a perpetual two-SF ceiling.

### S1 audit semantics: software receipt window and pre-TX proof (M-11/N-04)

S1 evidence uses **software RX dequeue/observation timestamps** from
`twai_receive()`, not CAN-controller or physical wire arrival timestamps.
The only claimed interval is `[request_tx_us, request_tx_us+1,000,000us)`,
where `request_tx_us` is the driver-accepted TX software timestamp. Hardware
RX timestamps are **not available**, so `complete=1` cannot prove the
physical arrival time of any frame. This limitation remains even with an
independent code review, and it cannot be promoted into an on-wire latency
guarantee.

Immediately before its single authorized S1 TX, the CAN owner drains
already-queued TWAI RX and processes it as `outside_pre`, then checks
`twai_get_status_info().msgs_to_rx==0` under the STOP/TX exclusion gate.
An unprovable empty queue, driver status error, or unbounded drain means
**no TX and `complete=0`**; no retry is permitted. The software elapsed gap
from empty check completion to driver acceptance is recorded as
`pre_tx_gap_us`. The cap is **500 microseconds**: a larger gap yields
`complete=0` (and if excessive before submission, TX is withheld).
`pre_tx_drain_frames`, `pre_tx_queue_empty`, `pre_tx_drain_end_us`,
`pre_tx_gap_us` and `pre_tx_gap_ok` must all be retained in the raw
`#SIGNALV2_S1` evidence. A queued frame arriving after the final empty
check but before driver acceptance is still a **residual physical-time gap**.
The residual gap is not proven empty by software and is **never** described
as physical-bus coverage.

`candidate_rx` is the number of candidate response-ID frames (`18DAF1xx`) whose
**software receipt timestamps** fall inside the 1-second interval.
`candidate_rx_total` is the entire S1 audit epoch, including pre-TX and
post-window candidate RX; these numbers must not be conflated. Record
`outside_pre`, `outside_post`, `blind_*`, driver losses, and
`owner_confirmed` alongside them. `complete=1` denotes only bounded,
accounted **software-visible evidence**, never a physically lossless bus
trace. S1 remains single-shot and Invalid; S2/S3 remain barred pending
separate owner authorization and evidence review.

## 5. S2 — stationary/P, 5Hz for at most 30 seconds

Only after owner+PM accept S1 and the owner explicitly authorizes S2. Start a **new** ARM epoch, discard the S1 pin and stale results, acquire the approved physical ECU again with no functional request. First valid physical read must yield genuinely Fresh speed **zero** and usable RPM within 3,000ms or automatic STOP/Invalid; no synthesized speed zero. Continue only **30 seconds maximum** at 5Hz under full admission, with all-RX and `18DAF1xx` counts, TWAI error/retry/drop, source/age/epoch evidence and automatic STOP on any unexpected frame. Compare speed and RPM visually to **the vehicle's original instruments** with timestamps; unavailable or unreadable stock RPM => **UNCONFIRMED**, not approved. Owner/PM review before S3.

## 6. S3 — driving (separate owner approval required)

Only after S2 review and explicit owner authorization. Begin with a new epoch and a fully verified stationary first 3-second acquisition *before driving*; never reuse the S2 pin. Drive only in an owner-approved controlled setting, two people with the driver never operating equipment. Continuously require Fresh real CAN speed, the current pinned ECU, two-level admission, no unexpected reply and clean monitoring. Visually compare the original car speed/RPM indications with the adapter where available, allowing for display latency; do not claim accuracy if original RPM is unavailable. Fault/unknown speed/budget breach/counter loss => STOP/Invalid, the driver parks safely, and no automatic re-ARM. A stationary pass does not itself authorize S3.

## USB ARM, renewal, STOP and abort procedure

ARM lasts **600,000ms (10 minutes)** and requires a currently eligible secured
session. Console close/USB host loss is **not** an immediate disarm event in
this implementation: assume TX remains eligible until expiry. Keep the owner
console and adapter self-monitoring log present. BLE disconnect revokes authorization;
reconnect requires a new explicit ARM. Renew only parked, with live verified
peer/profile, valid speed and clean load/error evidence; log ack and new expiry.
A renewal of an already-armed trial must not reset ECU binding/generations.
If authorization expires, stop the trial, verify no-TX, and start a new explicit
parked lifecycle; never renew while driving or use repeated ARM as recovery.
Bench expiry verification must actually wait the full 10 minutes.

`SIGV2 CAN STOP` first acknowledges `ack=REQUESTED` (revocation requested).
Wait for `ack=STOPPED` after outstanding work drains; record epoch,
SourceGeneration and ModeEpoch. The first ack alone is not proof of quiescence.
Check the adapter's own TX and FC submission counters for zero new diagnostic attempts for at
least 2 seconds after drained acknowledgement; classify this as self-observation, not independent physical proof. Preserve any already-on-wire,
late ECU or uncertain retransmitted traffic separately. STOP cannot recall a frame already transmitted.
If acknowledgement or no-TX proof fails, end trial and safely remove adapter
power/connection while parked. Never interact with equipment as the driver.

Abort on any warning lamp, CAN error/BUS-OFF/overrun/drop, diagnostic cap breach,
changed/ambiguous ECU, 11bit diagnostic reply, absent/Invalid speed outside the
bounded stationary acquisition phase, stale Fresh, failed monitor or recovery.
Second operator sends STOP; driver parks safely immediately as circumstances
allow. End this trial; disconnect equipment parked. No autonomous functional
recheck/repin. A new trial requires explicit authorization and all parked gates.
Restore the verified original firmware if recovery fails or the trial ends:

```sh
# ROLLBACK BRANCH ONLY, parked/offline, original board and active slot reverified.
python3 -m esptool --chip esp32s3 --port "$PORT" write_flash "$APP_OFFSET" original-app.bin
python3 -m esptool --chip esp32s3 --port "$PORT" verify_flash "$APP_OFFSET" original-app.bin
python3 -m esptool --chip esp32s3 --port "$PORT" read_flash "$APP_OFFSET" "$APP_LENGTH" restored-app.bin
cmp original-app.bin restored-app.bin
```

Reboot and verify original build/behavior; do not call rollback successful on
a write message alone. Save explicit entry/exit pass/fail and duration for each
phase, firmware/source/env/partition hashes, arm/STOP acks, ECU binding and
epoch changes, **all RX and 18DAF1xx per-ID/per-frame-type counts**, actual TX/FC submissions, TWAI
errors/drops/overflows and retries where observable, current/peak **reservation** windows and
self-estimated rolling load (200 bits/frame), stock-speed/RPM visual comparison where available,
per-PID age/Hz/quality, display evidence and warning-light status. Redact identities and locations before sharing.

Software load labels: `reserved_*` are conservative admitted request/RX/FC
reservations retained through the response interval. `charged_traffic_*`
uses actual candidate diagnostic RX timestamps plus precharged request/FC
attempts at 200 bits/frame (failed TX attempts remain conservatively
charged). **This is adapter self-measurement, not an external bus-analyzer trace.** Re-transmissions may not be exactly derivable from TWAI counters; if accounting is uncertain, STOP and reject the evidence. `actual_candidate_rx_frames`
counts all candidate-route RX including late frames;
`actual_fc_submissions` counts successful FC driver submissions, not proven
on-wire delivery. `4294967295` in a charged window/peak means saturated
accounting, an abort condition, never a valid measured load. Current windows
are recomputed even disarmed/idle, and peaks are retained for the boot.
