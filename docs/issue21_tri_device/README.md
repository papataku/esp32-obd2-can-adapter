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
