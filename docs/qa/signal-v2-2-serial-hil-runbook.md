# Signal V2-2 serial HIL runbook (N1 PR-D substitute)

Owner decision, 2026-10-29: serial HIL replaces the external analyzer for the
**software** obligations in PR-A §6. A build or host PASS is not a device PASS.
No electrical, arbitration, ACK/retransmit or real-ECU claim follows from HIL.


**m-07 scope boundary (PM decision, 2026-10-31):** The current serial-HIL
acceptance oracle is scoped to *full physical GROUP acquisitions* with a
causally verified FF, one FC, all CFs and a completed GROUP sample. Although the
production CAN owner can shed auxiliary reads and perform an 800-bit
**essential-only** (RPM/speed, no GROUP/FF/FC) acquisition when the budget is
tight, this HIL oracle intentionally **does not qualify that path**.
Essential-only samples are **FAIL / not qualifying HIL evidence**, even if the
firmware obtained them correctly. This is a declared test-scope limitation,
not a claim that the firmware must reject essential data. Extending evidence
and a dedicated essential-path state machine requires a **separate PR**; do not
relax the GROUP oracle or reinterpret a full-GROUP PASS as essential coverage.


## Image and isolation

Use only `m5dial_signal_v2_can_29bit_serial_hil`, never on a vehicle. Production
CanOwner, CanMonitor, budgets, leases and epochs run with the serial driver.
HIL is headless: it does not call M5Dial/M5Unified/M5GFX, use their cached board
identity, initialize touch/display, or probe CAN GPIO13/15. CI checks both the
absence of **all** physical `twai_*` symbols and display autodetection symbols
with `check_signal_v2_elf.py --hil`. Other environments keep their existing UI.

**B-01 (PM decision, 2026-10-31):** The serial-HIL environment is
deliberately separated from the vehicle-capable
`m5dial_signal_v2_can_29bit_5hz_lab` and
`m5dial_signal_v2_can_29bit_10hz_lab` environments. It extends the
CAN-disabled V2 lab foundation but explicitly replaces the V2-1 hard-disabled
build flags with the V2-2 test-only CAN owner and
`M5CAN_SIGNAL_V2_SERIAL_HIL=1`. The serial-HIL image intentionally does
**not** define `M5CAN_SIGNAL_V2_STAGE_GATED`, so a secured
`SIGV2 CAN ARM` can exercise a complete GROUP/FF/FC/CF/Fresh acquisition
**only via the USB virtual ECU**. This exception is permitted **solely if**
the actual built serial-HIL ELF passes the mandatory CI
`check_signal_v2_elf.py --hil` linked-symbol check, proving absence
of linked `twai_*` physical-driver entries and display autodetection.
A failed, omitted, skipped, or stale ELF check is NO-GO. This image
must never be flashed to a vehicle-connected board.

All V2 CAN environments with a potential physical TX route retain
`M5CAN_SIGNAL_V2_STAGE_GATED=1`; no generic ARM can authorize physical
CAN TX. CI evaluates the effective PlatformIO build flags of **every**
V2 CAN environment, and the GATT source also raises a compile-time error
for an unstaged non-HIL V2 CAN image. The host B-01 integration test compiles
the real CanOwner with the serial-HIL environment's effective macros and
checks ARM, physical-addressed GROUP submission, and a Fresh 5-PID sample.
Neither the host simulation nor the ELF symbol check proves electrical
behavior or a real BLE Central session; the latter remain separate device
acceptance gates.

The driver publishes fixed-size events into a bounded ring under the same
spinlock as RX/lifecycle state. The CAN owner never writes USB. The USB task
retains partial writes; full ring/truncated events latch an unrecoverable
transport fault, increment loss and refuse further TX. Reinstall cannot clear
that latch. A fresh capture requires reboot after a fault; `HIL RESET` is refused.

Every `#HIL_EVENT` has `seq,t_us,gen,lost,boot,kind`. TX includes failed submission
attempts (`ok=0`); RX reports board acceptance (`ok=1/0`), not host write time.
`gen` is the installed driver's generation; `boot` is a random boot identity.
`HIL RX <generation-decimal> <id-hex> <ext-0|1> <data-hex>` must target the observed
current driver generation (observed in board events) at delivery. The admission lock checks generation and RUNNING state together;
stale commands are rejected even after stop/drain/reinstall. A stopped driver's
already queued RX remains readable with zero wait. The lock contains no USB,
queue API, allocation or delay. A rejected stopped/stale injection is recorded
but does not invent controller RX loss.

Commands: `HIL FENCE <nonce>`, `HIL STATUS`, `HIL TXFAIL n`, `HIL BUSOFF`,
`HIL BUSERR n`, `HIL RXMISS n`, `HIL OVERFLOW`. FENCE echoes a new host nonce
through the ordered board event stream; it never resets evidence. OVERFLOW
atomically fills the configured 256-frame queue plus one rejected frame and
records accepted/rejected totals at one board timestamp. This deliberate RX
overload must STOP; its >cap load remains in JSON and is never called within-cap.
All normal traffic and **both** reservation gates retain 200 bits/frame,
5,000/100ms and 25,000/1s. No cap or budget algorithm is changed.

## M-19/M-20: boot and STOP generation evidence (2026-10-31)

The board's sample source-generation counters are **not** the TWAI driver
\`gen\` field. Both owner counters start at 1 (CanSamples, 
\`include/signal_v2_can_samples.h:17\`). The GATT authorization epoch starts
at 1 (\`src/signal_v2_gatt.cpp:47-48\`), and \`CanOwner::tick\` increments
**both counters once per changed authorization epoch**
(\`src/signal_v2_can_owner.h:182-186\`).

Before *each boot's* first FENCE the runner sends \`SIGV2 CAN STOP\`. It must
retain the matched board \`stop_request\` and \`stopped\` event as a separate
\`initial_stop_evidence\` JSONL record, including boot, request/ACK sequence
and timestamp, authorization epoch, source_generation and mode_epoch.
The initial STOP ACK is **not a post-FENCE board frame** and does not count
toward the 60-second session or the sequenced load ledger. A loose serial
\`ack=STOPPED\` line, a stale pre-command ACK, a missing prelude, a second
prelude for the same boot or an incorrect boot ID cannot qualify.

The mandatory generation transitions are:

- **Initial STOP then first ARM:** firmware initializes both counters at
  (1,1), consumes authorization epoch 1 in its first tick to (2,2),
  then increments once for the runner's initial STOP (normally epoch 2,
  generations 3/3) and once for ARM (normally epoch 3, generations **4/4**).
  Repeated *recorded* startup STOPs may increase the baseline; the matched
  ACK, not a guessed 2/2, is authoritative.
- **Ordinary operator STOP then re-ARM:** STOP increments the epoch and both
  counters once; the ACK must equal the last valid counters +1/+1.
  A subsequent ARM increments them once more.
  Source: \`src/signal_v2_gatt.cpp:166-205,599-611\`.
- **Internal owner fault then STOP ACK:** owner \`stop()\` increments both
  counters once (\`src/signal_v2_can_owner.h:38-48\`) and the first STOP ACK
  records that state. GATT also increments its authorization epoch after
  \`owner.tick\` returns. The next idle tick consumes this pending transition,
  adding one increment **before** a later operator STOP or new ARM
  (\`src/signal_v2_gatt.cpp:599-611\`).
- **Central disconnect then STOP ACK, later reconnection:** disconnect
  increments the authorization epoch (\`src/signal_v2_gatt.cpp:536-543\`);
  the next owner tick adds one increment and the STOP ACK records it.
  Reconnection alone does not authorize CAN TX; a new accepted ARM
  increments the epoch/counters again. Do not promote pre-disconnect data.
- **New boot:** counters reset to (1,1) and GATT authorization restarts at 1.
  A **separate initial STOP request/ACK and FENCE** for the new boot are
  mandatory. Never carry the preceding boot's counter baseline forward.

The oracle checks every same-boot STOP ACK **even if no later sample exists**,
enforces exact +1 (including the internal-fault deferred tick case),
checks request/ACK epochs and ordering, and rejects missing, nonnumeric,
out-of-range or discontinuous generations with a structured FAIL.
Full-GROUP-only scope (m-07) still applies; this is **software HIL evidence**,
not a physical-bus or central receipt test.

## Device procedure (separate authorized bench task)

1. Back up/verify the target identity and flash only with separate operator
   authorization. This PR's verification does not flash or touch a device.
2. Connect an authorized secured V2 central with ongoing 5Hz demand. For
   `rearm`, also request correlated Snapshots periodically (at least every
   100ms), through the invalid interval and the new ARM. Missing central
   activity makes the scenario fail; it is not waived by a pin log.
3. Install pyserial and run, with a unique evidence filename:
   `python3.11 tools/signal_v2_hil_ecu.py --port /dev/cu.usbmodem1101 --scenario normal --seconds 60 --trace normal.jsonl > normal.json`
4. Reboot before each independent case. Run healthy `normal`, `split`, and
   finite `admission`; discovery faults `noreply`, `extra`, `ff_probe`,
   `cf_probe`, `malformed`, `late`, `duplicate`, `wrong_src`, `eleven_bit`,
   `negative`, `txfail`; post-acquisition faults `busoff`, `buserr`, `rxmiss`,
   `overflow`, `overlap`; and `rearm`, `disconnect`, `reboot`.
5. For `disconnect`, manually disconnect the central after the first successful
   acquisition and within three seconds of ARM. The trace must show the actual
   GAP disconnect, STOP ACK, and no subsequent TX. Do not reconnect/re-ARM.
6. For `reboot`, manually reboot during acquisition after its first successful
   sample, within the first ten seconds. The same serial port must reappear
   within ten seconds. Reconnect the secured central with demand promptly;
   the runner fences the distinct boot, waits up to ten seconds for a read-only
   `SIGV2 CAN READY` confirmation, explicitly ARMs again and observes
   another 60 seconds. Two samples/identities, new discovery reservations and
   no promotion across the boot boundary are required. Failure to reconnect,
   an unexpected extra reset, or a missing boot identity fails the run.
7. Keep the JSON result **and complete JSONL trace**, including load lines,
   host writes/arrival times, board clocks, generations, losses, failed attempts,
   RX refusals and the final acknowledged STOP/quiet/fence. Do not commit raw
   logs containing device/peer identifiers. An exception/partial trace is FAIL.

The runner first waits for STOP ACK, fences flushed input using a fresh nonce,
then requires ARM acceptance. Fault injection is never replaced by a generic
STOP line. Faults retain their behavior on every probe, while any unexpected
retry fails accounting. Post-pin driver faults require a healthy group/FC/CF
sample first. Normal/split require sustained decoded speed=0, RPM=1000, current
source/epoch, fresh observation times and 5Hz cadence; a pin alone cannot pass.
All cases observe at least 60 seconds plus final STOP ACK and >=1s quiet.
The finite admission case stops after its first group and checks each gate's
first reservation=800, and all its recorded reservations <=4,000/100ms and
<=5,600/1s. The remaining interval is a quiet soak, not sustained acquisition.

`rearm` stops while group completion is pending, waits for the stable STOP ACK,
injects the old-generation CF, observes its rejection and an invalid Snapshot,
then re-ARMs into a distinct epoch. Fresh old results/packets after STOP are
forbidden. A new-epoch acquisition and Fresh correlated Snapshot are required.
`overlap` stops a pending group, then injects its old CF while the new epoch's probe is active. Any stale Snapshot
or missing Snapshot evidence prevents qualification. Snapshot telemetry records
successful notification submission, not receipt by the central; retain the
central's correlated Snapshot capture as separate end-to-end evidence.

Evaluator correlation (re-review R1-R3): a fault or disconnect qualifies only
with the **first completed STOP after it**, before any operator STOP request,
within 3s and into a later authorization epoch; the final operator STOP/quiet
is checked separately. `overlap` requires an Invalid Snapshot between the first
STOP and re-ARM and another after the fault; every Fresh Snapshot must carry
the active ARM epoch and generations newer than the last STOP, independent of
the board's `current=1`. Admitted RX must match a host write of the same
generation, and every non-`txfail` TX (including FC) must report `ok=1`.
The final operator STOP must be requested after every fault/disconnect and
complete before the >=1s quiet fence. `overlap` requires the old CF to be
admitted between the new epoch's probe and the fault. Fresh Snapshot checks use
the generations carried in the notified PDU records (`pdu_source_generation`,
`pdu_mode_epoch`, HIL-only telemetry), not the owner's current counters. A
scheduled injection refused (`ok=0`) after the fault's completed STOP is the
expected fail-closed outcome; any other refusal fails transport.

## Model boundaries and required evidence

Host regressions exercise the **real shim plus production switchDriverMode**,
configured capacity, stopped drain, BUS_OFF refusal, reinstall counter reset,
old-generation admission and transport saturation. They also check incomplete
and adversarial evaluator traces. They cannot establish ESP32 task scheduling;
run the rearm/overflow cases repeatedly (at least 100 rearm cycles in separate
captures) on hardware and retain each failure, not just a successful retry.

The model supports installed/start/stop/uninstall, NORMAL/NO_ACK/LISTEN_ONLY,
configured RX capacity (1..256), stopped RX drain and synchronous submission
failure. Start clears RX; reinstall recreates counters. BUS_OFF refuses RX/TX.
It is intentionally not an SDK replacement: timing/filter-register validation,
RTR/self injection, malformed DLC >8, controller queues/completion, arbitration,
TEC/REC, ACK/retransmission, timeout/recovery and electrical timing are not
modeled. Malformed ISO-TP data is injectable. TXFAIL tests submission rejection,
not on-wire failure or retries. NO_ACK here is only virtual successful submission.
Use hardware-specific tests for every unmodeled property; never infer them from
HIL PASS. Board event time measures virtual submission/acceptance, not a wire.
USB scheduling delay remains visible through the board response/sample times;
the evaluator cannot substitute the host's arrival/write timestamps for them.

S1/S2/S3 remain separate human-authorized vehicle gates. No result here proves
real ECU response count, actual CAN pin behavior on an attached transceiver,
central receipt, or physical retransmission accounting.
