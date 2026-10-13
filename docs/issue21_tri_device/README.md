# Issue21/95 physical BLE investigation

The accepted main baseline is a272c105 (flashed base04fa28d):
`m5dial_dual_ble_multipid_bench` uses NimBLE with TWO isolated BLE peers.
Issue21's original global-buffer/flag inspection and PR22's Bluedroid branch
are an older path. Do not replace the dual image with that standard profile
for a two-peer comparison or merge the old diagnostic branch blindly.
SmartRing and iPad are TWO centrals connected to the adapter's Nordic UART
GATT service; USB only supplies power and diagnostic capture. A connected JC
is a third central and must be explicitly disconnected for this scenario.

## Historical evidence

The iPad SQLite integrity check returned ok. Six adapterChanged collection
stops match M5Dial boot18 peer2 GAP disconnect records. Aligning the first
pair as an offset gives subsequent residuals from -0.004 to -0.128 seconds.
This is a fitted offset, not independent clock synchronization. It supports
actual peer disconnects rather than app-only state changes, but does not
identify who initiated teardown or why. The retained firmware did not keep
reason codes, and the SQLite did not log CoreBluetooth callbacks.

| iPad UTC | M5Dial boot18 milliseconds | residual after first-pair alignment |
|---|---:|---:|
|08:01:04.019|230050|0.000s|
|08:08:11.663|657698|-0.004s|
|08:12:18.442|904600|-0.127s|
|08:14:24.113|1030152|-0.008s|
|08:19:45.422|1351578|-0.125s|
|08:21:09.302|1435461|-0.128s|

Raw personal vehicle captures and complete device identifiers stay in local
artifacts, not Git. The two additional adapter disconnects lack corresponding
adapterChanged events and must not be silently dropped or called a cause.

## Diagnostic changes

NimBLE-Arduino1.4.x's onDisconnect callback does not expose the reason.
`setCustomGapHandler()` registers a raw observer. It copies the reason,
connection handle and link parameters into a64-slot POD ring under the
existing mutex, and never changes session/lease/query state. The main loop
prints at most8 events per pass. Overflow is counted and sequence gaps remain
visible. Callback Serial printing was removed. Existing20-byte unicast chunks,
CAN/ELM policy, cache, per-peer generations, and two-peer limit are unchanged.

`reason` is the RAW NimBLE host integer, not an8-bit HCI value. For values in
its HCI namespace, `BLE_HS_ERR_HCI_BASE=0x200`. For example531=0x213 is HCI0x13
(remote user connection termination), and520=0x208 is HCI0x08(supervision
timeout). Other namespaces must retain raw values as UNKNOWN unless the
installed stack defines them. Link interval units are1.25ms, supervision
units10ms. Zero parameters in CONNECT rows mean unobserved, not a0ms link.
RSSI/PHY/subscription/airtime acknowledged delivery are not observed by this
patch; notify counters describe successful host queue admission only.

## Reproducible Mac capture

Use Python3.10+ with pyserial. Identify each USB serial number first. The
runner verifies303a:1001 and exact known serial numbers before opening.
No two processes may open the same serial port. Opening may cause a USB reset;
startup epochs are recorded separately from the measurement interval.

```sh
python tools/tri_device_ble_soak.py \
  --adapter /dev/cu.ADAPTER --ring /dev/cu.RING --jc /dev/cu.JC \
  --output /private/path/unique-run --seconds 1845 \
  --scenario ipad-ring-batch --demo-auto --isolate-jc \
  --fw-sha EXACT_SOURCE --app-sha EXACT_APP_SOURCE \
  --ipad-device DEVICE --ipad-scenario batch --ipad-seconds 1800
```

DEMO AUTO must reply active=YES; otherwise capture stops and no experiment is
admitted. This uses the existing setting (persisted in NVS); it is not forced
DEMO on a live bus. Do not connect a vehicle for unattended testing. Ring's
runtime starts after the splash; isolation commands are delayed until it can
accept them. Save settings before experimentation and restore the prior
DEMO/client configuration after testing. Current baseline was DEMO OFF.

With --ipad-device, the runner launches the already installed companion AFTER
DEMO verification, keeps both ports open for the whole window and retrieves its
SQLite/JSONL at the end. Launch failure is reported explicitly; no unlock bypass
or automatic approval is attempted. Capture duration must exceed app duration
by at least30s. Manual launch is also available:

The iPad companion debug-only app in the Honda Analyzer issue branch accepts:

```sh
xcrun devicectl device process launch --device DEVICE --terminate-existing \
  com.papataku.HondaAnalyzer --ble-soak --soak-scenario batch \
  --soak-seconds 1800 --soak-run-id unique-run
```

`idle`,`single`,`batch` are
supported, each bounded to3600s, one ELM command in flight. No reconnect,
unknown DID discovery, driving collection or write commands are automatic.
The app verifies identity/capabilities and MODE=DEMO before any PID reads.
Existing sessions are backed up before app installation, never uninstalled.
Capture goes to a separate Documents/BLESoak directory, withSQLite and JSONL.
After completion, use devicectl device copy from to retrieve that directory,
includingSQLite WAL files, and check integrity. JSONL records independent
UTC+device monotonic time, BLE transitions, NSError reason, app scene changes,
samples, failures and end. Use the same run ID in both host/app manifests.

A locked iPad can be enumerated/copied/installed while launch is refused by
SpringBoard. Request unlock only after the app is prepared, then launch
immediately. The debug test disables foreground idle lock during the run and
restores its prior value on completion. It cannot override manual lock or
background behavior. A refused launch is HARDWARE_BLOCKED, never PASS.

## Results and gates

60s iPad+SmartRing smoke:203 five-PID batches,220/220 command successes,
SQLite integrity ok, no unexpected disconnects or notify failure/retry.
The end of the app calls disconnect intentionally and produced raw531; the
SmartRing generation stayed connected and fresh samples continued. JC's
intentional disconnect also produced531. These verify logger/peer isolation,
not the cause of the historical unsolicited cuts.

The first requested iPad smoke was blocked by device lock. Ring isolation was
sent before its deferred runtime was ready and was ignored. That capture is
setup/SmartRing traffic evidence, NOT an iPad-alone or30min PASS. The runner
now delays isolation until runtime readiness time; a later mixed capture was
intentionally interrupted to prepare a separate combined30min experiment.

Full physical acceptance remains single and combined30min x3 repetitions,
idle/single/batch and controlled fault scenarios, unknown disconnect0,
reboot0, no response mixing, SQLite ok, and fresh per-PID samples. CI/DEMO
does not establish real CAN/vehicle acceptance.

Offline analysis:

```sh
python tools/analyze_tri_device_soak.py --capture RUN_DIR \
  --ipad-jsonl RUN_DIR/ipad-bundle/RUN_ID.jsonl \
  --ipad-db RUN_DIR/ipad-bundle/RUN_ID.sqlite3 --output RUN_DIR/analysis
```

This reports the measured window, latency percentiles, resets/disconnects,
per-PID updates/age and per-peer throughput. Missing coverage remains unknown;
the script does not issue a blanket physical PASS.

## Interpreting the deployed status and connection order

In dual mode the USB SerialProtocol intentionally has a null legacy BLE
transport. Consequently #STATS,ble=WAIT can coexist with2 active clients; use
#BLE_DUAL/#BLE_PEER and the meter BLE state as link evidence, not that legacy
USB field. The adapter display uses the actual aggregate transport.

Connection order is distinct from handle/slot. In the Ring-first comparison,
JC initially used handle1 and was explicitly disconnected; SmartRing kept
handle2/generation2, while the later iPad reused handle1/generation3. A
"handle2 always fails" theory cannot be equated with"later peer fails".
For the reverse order use --ring-after-ipad together with --ipad-device: start
with Ring isolated, verify the iPad's DEMO-capability event, then c resumes
Ring's one saved verified M5CAN candidate. No ambient scan or alternate ELM
selection is performed. Verify actual CONNECT rows and fresh samples; launch
order alone is insufficient. Compare the same firmware, rates and window.

Public reports list5 review angles performed by the author, not fictitious
independent reviewers: BLE APIs/routing, RTOS/resource bounds, CAN safety,
SQLite/RAW persistence, and test/clock/UX reliability.


## Failure fixture and current acceptance limits

A normal30min simultaneous trial completed6026 five-PID batches with no
unexpected disconnect, runtime reset or notification failure. It had81 iPad
notify retries and70 Ring retries, and p95 batch RTT115ms. This is not a
zero-congestion result. Ring RPM effective update rate was8.826Hz under this
load (target10Hz), hidden signals2Hz. Interleaved USB signal diagnostics
produced27 unparseable rows; they remain UNKNOWN rather than fabricated ages.

The user's original condition includes unknown DID exploration. The raw
iPad SQLite contains130 NRC0x31 and1 NRC0x33 responses. Host tests replay
these, NO DATA, partial positive/BUFFER FULL and missing-prompt timeout.
The physical no-data fixture uses the same installed adapter DEMO image and
a finite three-DID error batch followed immediately by five known positive
PIDs. This differs from a real CAN ECU timeout/ISO-TP workload, so a passing
fixture cannot exclude CAN-side blocking or the original full analyzer UI.
Compare Ring-first/iPad-first for the same600s error-mixed load and record
actual handles/generations (not merely launch order). Record successful
normal batches separately from expected negative items; no unknown-vehicle
discovery or write/security/session commands are introduced.

Meter image identity is observed from TACOLOG ready (full ELF SHA), but the
matching saved source ELF has not yet been located. Keep the SAME unmodified
Ring image across order trials, publish this provenance limit, and do not
claim the current saved pre-drive ELF matches a differently fingerprinted
installed image. Adapter/app first trial binaries and source basis are stored
locally; later app error fixture is a clean signed source63b6c77.


Ring-first NO DATA physical trial completed600.145s:1666 negative three-DID
batches (4998 NO DATA items), each followed by a successful five-PID batch.
No unexpected disconnect/reset/notify failure during the app window;
SQLite integrity ok and complete host coverage. Ring RPM9.208Hz, hidden
signals approximately2Hz;9 malformed USB rows excluded. The end disconnect
raw531 was intentional app cleanup. The initial reverse-order launch was refused by
SpringBoard Locked; after user unlock the reverse600s trial completed below.

For explicit error injection only, `m5dial_dual_ble_fault_fixture` inherits
the same two-peer profile with physical CAN TX compiled out. Compilation
fails if FAULT_FIXTURE is enabled without CAN_MODE_DRY_RUN. Only verified
DEMO responds to22FFFD with NRC31,22FFFC with NRC33,222012 with incomplete
ISO-TP first frame+BUFFER FULL, and22FFFE with no bytes/prompt. Normal UDS
still returns NO DATA. The app `faults` scenario requires FAULTS=1 and
MODE=DEMO, expects a300ms command timeout, then verifies a fresh five-PID
batch without reconnect/reinitialization. This dedicated image was subsequently
installed for the bounded physical trials below, then removed. Host fixture confirms peer
isolation, subsequent responses and zero submitted CAN requests;47 Swift
unit tests and iPad unsigned device compile passed. Restore the captured
adapter image/configuration after physical testing. These fixtures simulate
application-visible error replies, not real ECU/ISO-TP electrical behavior.


## Completed load/order/error trials (2026-10-12)

| Trial | window | successful five-PID batches | negative fixtures | outcome |
|---|---:|---:|---:|---|
| Ring first, iPad second NO DATA |600.145s|1666|4998 NO DATA items|no in-window cut/reset/notify failure|
| iPad first, Ring second NO DATA |600.175s|1662|4986 NO DATA items|no in-window cut/reset/notify failure|
| high-load NO DATA, repeats3/delay0 |180s requested|1695|1695 NO DATA items|no in-window cut/reset/notify failure|
| mixed errors, repeats8/delay0 |180s requested|1176|each of5 types147 times|no in-window cut/reset/notify failure|
| high-load mixed errors, repeats24/delay0 |180s requested|1776|each of5 types74 times|completed, one end-boundary cut noted below|

Five types are NO DATA, NRC31, NRC33, partial first-frame+BUFFER FULL, and
no bytes/prompt (300ms command timeout), followed by normal positive reads
without reconnect/re-init. Actual fixture uses TX-disabled adapter image
(source basis2c227a6-dirty), signed app5f622f9 (repeats3/8), then3542550
(repeats24). Main and the installed meter image were not changed.

The original six pre-disconnect60s windows were54.53-82.58 notifications/s
and1066.57-1593.22B/s. Earlier low-load NO DATA trial was40/s and746.67B/s;
it was NOT a sufficient reproduction of that traffic. Actual iPad SQLite
receive rates in every full60s high-load NO DATA window:111.63-119.73/s,
2069.77-2219.48B/s. For repeats8 mixed errors:74.13-77.75/s,
1360.83-1426.97B/s (within original range, below original maximum).
For repeats24 mixed errors:105.42-113.72/s,1941.73-2094.17B/s. All full
windows of both HIGH-load trials exceed the historical maximum in BOTH
notification count and bytes. The recorded scan phase itself had10.64/s
and161.59B/s; it is distinct from later collection/disconnect windows.
Logged command counts include synthesized batch item results and must not
be presented as wire request counts.

Repeats24 raw531 GAP disconnect was timestamped11:25:43.752812 Mac UTC;
iPad's final successful sample11:25:43.777 and SOAK_END11:25:43.782 UTC.
Both clocks are independent; the29ms boundary inversion is consistent with
intentional app cleanup but is not precise proof of initiating order. Keep
this event in the analyzer output instead of silently filtering it. The
app ended withfailed=false, SQLite integrity ok, complete host coverage,
no notify failure/reset. Future debug source recordsSOAK_STOP_REQUEST before
local teardown; that marker was NOT in the tested3542550 binary.

No claim of equivalent CAN wait/ISO-TP electrical behavior, full discovery
UI/CPU/storage load, full30min single-client/reverse-order matrix, or cause
resolution is made. Ring RPM was8.887Hz during high-load NO DATA and9.281Hz
during repeats24 mixed errors; hidden signals approximately2Hz. The10Hz
RPM target remains unmet. Order trials had iPad notify retries340 vs464,
Ring85 vs47; one trial each is insufficient for a causal order claim.

After testing, the adapter's captured initial diagnostic binary was restored
at0x10000 withflash hash verified. BOOT=a272c105-dirty, DEMO setting OFF,
active NO, saved YES confirmed. Dedicated fault image is no longer installed.
The iPad retains the signed3542550 diagnostic build and its normal/debug
paths; no app uninstall/data deletion. A normal-path relaunch was refused
bySpringBoard Locked after testing; no unlock bypass/retry was attempted.
