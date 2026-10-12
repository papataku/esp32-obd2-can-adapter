# Post-drive retained diagnostic journal

USB command: `DRIVELOG`. It prints `#DRIVELOG,BEGIN` through
`#DRIVELOG,END` without clearing saved history. Use
`python tools/collect_drive_log.py --port <current-M5Dial-port> --output trip.log`
after returning from the test. Verify the device MAC/USB identity before choosing
a port. Opening USB may restart the board: the previous boot remains readable.

NVS stores the most recent eight boots, including boots without CAN activity.
Each record has a schema/version/size check and an atomic NVS blob update.
Saved evidence includes build timestamp, grouped-build flag, reset reason,
uptime, grouped/individual request counts, accepted groups, fallback and stop
counts, disconnect count, connected peer address suffixes, DEMO state, CAN
RX/TX, query success/no-data/failure/timeout counts, CAN/driver drops/errors,
BLE RX overflow and notification failure/retry counts,
CAN/BLE task stack free bytes and heap minimum/largest free block,
grouped transaction count/total/maximum time (not BLE RTT).

The last eight state/fault events include time, connection ID, diagnostic status,
ECU ID, transaction/first-response time, response count and up to two original diagnostic RX frames. These are
original `DiagnosticResult.raw_frames`, not the synthesized per-PID ELM frames.
Event kinds: 1 grouped enabled, 2 fallback, 3 fault stop, 4 disconnect,
5 lease denied, 6 CAN fault lockout. Diagnostic status numbers follow `DiagnosticStatus` in
`src/diagnostic_policy.h`. Only the first successful group creates a success
event; all successful groups update counters.

Only the Arduino loop writes NVS; the BLE application updates a protected RAM
record. No full-bus logging or per-query flash write occurs. Checkpoint every
30 seconds, or at least two seconds after the last write when a state/fault event
is pending. `DRIVELOG` also forces a current checkpoint. Save failures emit
`#DRIVELOG,storage_error` and retain RAM evidence; persistence is best effort.
An abrupt power cut can lose changes since the last successful checkpoint
(normally up to 30 seconds). A new boot's reset reason explains how that boot
started; it does not label every power cut as a firmware crash. Uptime is relative
to each boot; the board has no trusted wall clock. Repeated USB restarts consume
history slots. Read the journal promptly after testing.

Grouped CAN remains restricted to the explicit
`m5dial_dual_ble_multipid_bench` build. Boot Listen-Only, finite TX leases, exact
read-only request policy, serialized CAN owner, fault lockout and per-PID pacing
remain unchanged. `DEMO AUTO` switches synthetic data off after real CAN RX;
logs show whether the recorded boot actually used CAN or remained in DEMO.
This journal does not establish physical CAN/vehicle compatibility or meter Hz.
