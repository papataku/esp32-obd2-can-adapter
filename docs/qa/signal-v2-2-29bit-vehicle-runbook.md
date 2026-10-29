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

This is an OWNER-OPERATED procedure. Agents must not flash, erase, connect to
real CAN, or operate a vehicle. First obtain independent review of exact
adapter and Ring SHAs, green host/policy/build CI, receiver freshness/epoch
acceptance, and the independent bench evidence below. **10Hz is excluded
from the entire first trial**, including after stationary success. Its
compile-only CI image is not permission to use it. Use matched explicit
5Hz environments and record actual per-PID rates; requested rate is not proof.

## N1 PR-A (docs-only): candidate approval and the three-second gate

See [the RP8 candidate evidence and bounded physical-discovery specification](../protocol/signal-v2-2-bounded-ecu-discovery-pr-a.md). The three iOS owner-log-derived docs prove five observed 29-bit reply IDs, but **not** the induced maximum of two Single Frames per physical `0C/0D` request. Only `18DAF101` is proposed for separate owner review as a current-RP8 priority; it is **not a TX permission or pin**. All other observed sources and RPM conflicts remain unresolved. Therefore **this PR-A does not alter NO-GO:** phases 4–6 and vehicle ARM remain barred.

For a future independently approved PR-B/C implementation only: explicit stationary ARM starts a NEW 3,000 ms epoch with all values Invalid; send individually admitted physical `0C/0D` only to owner-approved evidence candidates (<=4, currently proposed one), at least 200 ms apart, 120 ms response windows, no discovery FC or functional TX. Four frames/800 bits per candidate; retain 200-bit charges including late traffic and BOTH admission gates. Worst conservative envelopes: **4,000/5,000 bits per rolling 100 ms and 5,600/25,000 bits per rolling 1 s**, with preexisting reservations additionally enforced. Only current-epoch RAW-verified and independently approved unique ECU may pin; separately reserve first normal physical read and establish genuine Fresh speed zero by 3 s. Missing frame-count proof, source correctness, authorization, budget, independent analyzer or timely acquisition => STOP/Invalid, no automatic retry/functional fallback and no use of previous pin. Preserve phases 1–3 as read-only/offline evidence collection. No autonomous trial acceptance.

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

## 2. Mandatory independently instrumented isolated bench (before vehicle)

Entry: no vehicle CAN; controlled isolated simulator bus and independent CAN
analyzer with verified timestamps, loss/error counters and physical load
measurement. Software reservations are estimates, not measured utilization.
Do not claim this gate from host tests or compilation alone.

Capture at least 60s per supported bench case: boot/unarmed; authorized
acquisition; duplicate SF/group/FF; maximum responders; capacity exhaustion;
delayed/late replies across 100ms/1s boundaries; load pressure and speed-first
shedding; timeout; pin loss; STOP during request/FC; ARM expiry; BLE disconnect,
reconnect and immediate rearm. Fault injection belongs ONLY here.

Use the analyzer to distinguish adapter request/FC and induced diagnostic RX
from ambient traffic; include all those bits and CAN stuffing/interframe cost
in every rolling 100ms and 1s interval, not aligned buckets. Require diagnostic
load <=10%/100ms and <=5%/1s at 500kbit/s, no capture loss, no unexpected TX,
and genuine acquisition age/Invalid behavior on both ends. Account for late
traffic even after timeout/STOP. An external ECU flooding cannot be silenced
by the adapter; require latched adapter TX halt and reject the test result.
Confirm current functional discovery produces **zero** TX and a latched STOP;
that confirms refusal, not successful vehicle-acquisition qualification.

Exit: independent capture/report proving all limits and zero adapter TX before
ARM and after drained STOP/expiry/disconnect; exact hashes and reviewer sign-off.
Failure prevents any active vehicle trial.

## 3. Passive parked vehicle inspection — never ARM here (2 minutes)

Entry: phases 1–2 passed for the intended scope; owner verified wiring,
power/termination and healthy vehicle baseline. Park, P, parking brake, IG ON;
no other diagnostic tester. Keep a second operator and independent analyzer.
Boot unarmed, encrypted approved Ring may connect, but do not issue ARM.

Observe passive bitrate/address format, error counters and warning lamps.
Listen-only cannot solicit ECU discovery or prove Mode01 support. An ECU
response ID seen passively is evidence to review, not a pin/authorization.
Analyzer must verify zero adapter request/FC TX for 2 minutes. Any TX, warning,
bus error, overrun, dropped monitoring frames or unverified wiring aborts.
Exit: clean passive evidence. This revision stops here for vehicle work.

## 4. Explicit armed stationary acquisition (future gate, maximum 3 seconds)

Entry: discovery refusal resolved by separate reviewed change with a proven
induced-RX bound; repeat bench and review at new exact SHAs. Vehicle remains
P/braked, stationary, driver ready to end trial; no movement permitted.
Only after verifying encrypted, bonded, USB-registered subscribed peer,
active authorized HELLO/profile and 5Hz demand, locally issue `SIGV2 CAN ARM`.
The first admitted discovery request AFTER this ARM is the first possible
CAN diagnostic TX. Record arm ack, epoch, actual request/response IDs and
unique RAW-backed ECU pin. No functional fallback after pin loss.

Initial absent/Invalid speed is allowed solely inside this stationary
acquisition phase for at most 3 seconds after ARM. Any warning/error/load
violation/ambiguity/11bit response aborts immediately even during this phase.
Exit requires unique verified 29bit ECU and genuinely Fresh valid speed zero
with acquisition timestamp/sequence within admitted deadline. Missing/Invalid
speed at 3 seconds => STOP, end trial; no automatic repin/retry. No such grace
period exists during idle acceptance or driving.

## 5. Stationary IG ON then idle acceptance (2 minutes each)

Entry: phase 4 exit criteria and independent live monitoring satisfied.
At all times require Fresh valid CAN speed zero, same verified ECU and valid
RPM; auxiliary PIDs may be explicit Invalid after shedding, never false zero.
Measure each PID's actual Hz and age; inspect CAN/DEMO/Invalid labels. A
reservation denial/degradation is recorded and reviewed before progressing.
Observe analyzer rolling diagnostic load, total ambient load, warning lamps,
CAN error/overrun/drop counters continuously. Either operator ends the trial
on any abort. Reconfirm stationary acceptance after an explicit new ARM;
reconnect does not renew authorization or allow automatic recovery.
Exit: both 2-minute phases pass, saved independent load and sample-quality
records, no missing/Invalid speed or stale Fresh, explicit owner decision.

## 6. Low speed (maximum 2 minutes, only after all stationary gates)

Entry: independent review permits this phase, safe controlled route and agreed
low-speed ceiling; two people; second operator watches analyzer/logs and
records, driver touches no equipment. Require valid current speed continuously;
there is no discovery/recovery/invalid-speed grace period while moving.
No firmware/rate change and no 10Hz. Exit: park safely and STOP, then verify
no-TX and review evidence. A stationary pass alone is not low-speed approval.

## USB ARM, renewal, STOP and abort procedure

ARM lasts **600,000ms (10 minutes)** and requires a currently eligible secured
session. Console close/USB host loss is **not** an immediate disarm event in
this implementation: assume TX remains eligible until expiry. Keep the owner
console and independent monitor present. BLE disconnect revokes authorization;
reconnect requires a new explicit ARM. Renew only parked, with live verified
peer/profile, valid speed and clean load/error evidence; log ack and new expiry.
A renewal of an already-armed trial must not reset ECU binding/generations.
If authorization expires, stop the trial, verify no-TX, and start a new explicit
parked lifecycle; never renew while driving or use repeated ARM as recovery.
Bench expiry verification must actually wait the full 10 minutes.

`SIGV2 CAN STOP` first acknowledges `ack=REQUESTED` (revocation requested).
Wait for `ack=STOPPED` after outstanding work drains; record epoch,
SourceGeneration and ModeEpoch. The first ack alone is not proof of quiescence.
Check independent analyzer for zero new adapter diagnostic request/FC for at
least 2 seconds after drained acknowledgement; retain any already-on-wire or
late ECU traffic separately. STOP cannot recall a frame already transmitted.
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
epoch changes, actual RX/FC counts, current/peak **reservation** windows and
independent measured rolling load, per-PID age/Hz/quality, display evidence and
warning-light status. Redact identities and locations before sharing.

Software load labels: `reserved_*` are conservative admitted request/RX/FC
reservations retained through the response interval. `charged_traffic_*`
uses actual candidate diagnostic RX timestamps plus precharged request/FC
attempts at 200 bits/frame (failed TX attempts may conservatively remain
charged); these are not analyzer-measured bits. `actual_candidate_rx_frames`
counts all candidate-route RX including late frames;
`actual_fc_submissions` counts successful FC driver submissions, not proven
on-wire delivery. `4294967295` in a charged window/peak means saturated
accounting, an abort condition, never a valid measured load. Current windows
are recomputed even disarmed/idle, and peaks are retained for the boot.
