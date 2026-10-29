# Signal V2-2 serial HIL runbook (N1 PR-D substitute)

Owner decision, 2026-10-29: serial HIL replaces the external analyzer for the
**software** obligations in PR-A §6. A build or host PASS is not a device PASS.
No electrical, arbitration, ACK/retransmit or real-ECU claim follows from HIL.

## Image and isolation

Use only `m5dial_signal_v2_can_29bit_serial_hil`, never on a vehicle. Production
CanOwner, CanMonitor, budgets, leases and epochs run with the serial driver.
HIL is headless: it does not call M5Dial/M5Unified/M5GFX, use their cached board
identity, initialize touch/display, or probe CAN GPIO13/15. CI checks both the
absence of **all** physical `twai_*` symbols and display autodetection symbols
with `check_signal_v2_elf.py --hil`. Other environments keep their existing UI.

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
