# M5Dial bench DEMO touch switch (v0.4.4-phase3f-demo-touch)

**Scope**: M5Dial adapter only. Do not modify TacoTacoMeter/SmartRing's
existing DEMO vs live-BLE source selection. When testing the M5Dial-generated
BLE values on SmartRing, select **DEMO OFF** on SmartRing as before.

## Screen operation (no USB command needed)

The circular 240x240 M5Dial display has a highlighted touch row directly
below its large CAN/DEMO status line:

- **DEMO AUTO  TAP** (green): synthetic Mode01 read replies are generated
  only while zero physical CAN frames have been received since boot.
  Once any real CAN frame is received, the firmware stays in real-CAN mode
  until reboot. No demo frames are ever transmitted onto vehicle CAN.
- **DEMO OFF  TAP** (gray): fake replies are entirely disabled.
  Real CAN diagnostic replies are used if available; without a CAN
  connection, read requests may return timeout/NO DATA.

Tap the highlighted band (x=38..201, y=50..70). Each tap changes
AUTO <-> OFF. A 400ms debounce prevents accidental double toggles.
The main status remains **DEMO DRIVE** when synthetic data are actually
active, or reports CAN LIVE/IDLE/QUERY/ERROR for real CAN.
The selected setting is saved as NVS namespace `m5can_demo`,
key `auto`, and restored on reboot. On first flash with no saved
setting, default is AUTO.

USB terminal commands `DEMO AUTO` and `DEMO OFF` use the same saved
setting. Legacy `DEMO ON` is a backward-compatible alias for AUTO
(not a force-to-demo override). `ATM5CAP` still returns
`MODE=DEMO` or `MODE=CAN` based on the *actual* response source.

Touch processing runs on every main-loop iteration; status screen drawing
is still bounded at 250ms to avoid LCD overhead. No new BLE commands,
leases, arbitrary CAN TX, or shared SmartRing code are added.

## Bench acceptance

1. Flash `feature/m5can-bench-drive-demo`, PlatformIO environment
   `m5dial`. Disconnect the vehicle CAN/OBD wiring; use USB for power.
2. Verify default green `DEMO AUTO TAP`, main status `DEMO DRIVE`.
   SmartRing must show the M5Dial's active waveform when SmartRing's
   own DEMO mode is OFF and BLE is connected.
3. Tap the green band once: confirm `DEMO OFF TAP`, DEMO DRIVE
   disappears, and live values stop if no real CAN response exists.
   Tap again: AUTO returns and synthetic data resume.
4. Restart the M5Dial after selecting OFF, ensure OFF persists and that
   there are no synthetic CAN replies until manually selecting AUTO.
   Repeat with AUTO.
5. While BLE is connected and batches are being requested, toggle
   AUTO/OFF. Verify no BLE disconnect, display freeze or stale valid
   values after lost responses.
6. For controlled bench CAN input, verify that AUTO changes from DEMO
   to CAN after an actual received CAN frame (and does not revert
   to DEMO merely because traffic goes idle).
7. Capture touchscreen interaction on video along with M5Dial USB logs,
   firmware version, and SmartRing connection/reconnect metrics.

**Safety**: AUTO is only a bench convenience; OFF is recommended if
there is a risk of interpreting synthetic data as real vehicle readings.
Do not connect CAN to a vehicle just to test this switch.
