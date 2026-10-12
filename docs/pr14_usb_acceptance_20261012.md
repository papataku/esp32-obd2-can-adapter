# PR14 USB-only acceptance, 2026-10-12

Firmware code: `de160e03cee920e9764ae6a3d69aa12d2017b8af`.
20-byte dual profile SHA256: `66d340f250299b85544cc35997a96f12de51182c80fe620bc3d5dc6d13e00056`.
Only M5Dial was flashed; SmartRing/JC retain the previously verified fast
receivers and saved 10/10/10/2/2 Hz foreground / 2 Hz background policy.
CAN/OBD remained disconnected. This is USB-demo evidence.

## Findings and fixes

- The previous MYNEWT affinity macro was unused by NimBLE-Arduino1.4.3; use
  CONFIG_BT_NIMBLE_PINNED_TO_CORE and assert the actual compiled setting.
  Hardware logs confirm CAN owner core0, BLE worker core1, nimble_host core1.
- Peer suffix used the address prefix; extract little-endian val[1:0].
  One-at-a-time connections identify SmartRing DD41 and JC FB19. Slots vary.
- The user observed clipped top text. Its Font2 width161px exceeded the
  circular row width~109px. BLE n/2 is52px and retains the connection count.
- DEMO OFF reproduced two CAN-owner stack-canary resets during TWAI driver
  mode switching / esp_intr_free IPC. Move bounded result/transaction scratch
  into fixed single-owner storage; compiler frames drop3392+3040 to144+336.
  Queue/cache copies precede reuse; reset on failed TX too. CI guards both
  owner functions at512bytes and checks the8192byte BLE worker frame reserve.

## Functional and dry-mode checks

Both solo connections, reconnect generations, five dual page pairs and
configured foreground/background rates pass. Per-peer RX/TX are application
payload counters, not radio acknowledgements. Exact counter windows are in
the adjacent JSON; nominal page windows were45seconds after warming.

The dedicated dry profile has physical TX compiled out (no twai_transmit
symbol in its ELF). Two OFF20s/AUTO20s cycles execute1540diagnostic requests
without reset. Physical CAN RX/TX remain0; CAN stack minimum4416bytes, BLE
stack minimum4120bytes, internal heap minimum197964bytes. Error replies
during OFF are expected in this profile; they are not valid ECU data.

## Continuous run stopped as requested

The final normal20-byte run started09:03:02JST and ran1476seconds
(about24.6minutes) before the user explicitly stopped it. Both clients
remained connected; no reboot, ELM failure, RX drop, PID timeout/classified
error, queue overflow or notification retry/failure was recorded.
**This is not a65-minute or one-hour pass.** Long testing is deferred to the
final stage at the user request. The earlier failing run is not accepted.

| Meter | Counter window seconds | Visible signal average Hz | Lowest short interval Hz |
| --- | --- | --- | --- |
| SmartRing RPM | 1480.317 | 9.9864 | 8.9964 |
| JC speed | 1480.406 | 9.9567 | 8.5906 |

Hidden signals averaged~2Hz. JC still has brief8.59Hz intervals and a
sampled batch p95 up to145ms; CPU separation does not eliminate all jitter.
These maxima are not a controlled regression comparison: the previous
65-minute baseline sampled status every5minutes, this run every minute.

Adapter minimum internal heap197928bytes, CAN stack
5232bytes, BLE stack4120bytes.
BLE poll peak10549us; UI loop gap peak20008us.

CI38185527348 passes host regressions, normal/dual/MTU/dry builds and stack
checks on the firmware code. Live CAN cache sharing, actual ECU throughput
and final one-hour acceptance remain outside this completed short test.
PR14 is integrated into its declared PR13 parent branch; this does not
represent integration of the wider staged adapter phases into main.
