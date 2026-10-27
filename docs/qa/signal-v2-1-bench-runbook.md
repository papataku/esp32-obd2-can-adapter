# Signal V2-1 USB bench (CAN forbidden)

Source: Draft #58, `docs/v2/m5can-v2-spec-v3.md`, frozen v3.0.
Default `m5dial`, NUS UUID/wire `PROTO=1.1`, Log Stream V2 remain separate.
The opt-in peripheral has **no TWAI init or transmit symbol**. V1/USB vehicle
requests return `M5CAN TX LOCKED`, including when saved DEMO is enabled.
No vehicle connection or CAN-load/5Hz LIVE qualification is permitted.

## Build and backup gate

Run `./tests/run_all.sh` with Python >=3.10, then:

```
pio pkg install -e m5dial_signal_v2_can_disabled_lab
pio run -e m5dial_signal_v2_can_disabled_lab
pio run -c platformio_smartring_v2_lab.ini
```

The pre-script patches only each lab env's pinned NimBLE 1.4.3 local copy:
repeat-pairing and missing-key errors preserve old bonds; full bond storage
returns an error rather than evicting an owner. Four registrations/bonds,
two connections. Just Works is **not MITM proof**; USB identity verification
and explicit `APPROVE` are required. No CAN permission can be granted.

Before writing, rediscover MAC and USB serial for each physical board. Read
**all** flash, verify the on-chip hash, store SHA256 and partition/OTA metadata.
2022/2023 OpenOCD produced corrupt USB-JTAG reads on this Mac. The official
Espressif 2026-08-31 build retries CRC failures and full-bank hash verification
is mandatory. A size-correct file or esptool timeout is not a backup pass.

Use `tools/signal_v2_flash_check.py --target m5dial|smartring --backup ...
--firmware ...` to validate the image SHA, installed app capacity and active
OTA selection. Never run PlatformIO upload, full erase, partition upload,
OTA migration, or a default release image as part of this experiment.
Write **only the selected app** using verified target-specific JTAG serial
and `program_esp <firmware.bin> <installed-app-offset> verify reset exit`.
Both observed selected apps start at 0x10000; this is **not a universal default**.
Save exact source SHA, firmware hash, verification and post-boot banner.

## Local registration

USB M5Dial commands (not available via NUS):

```
SIGV2 PEERS
SIGV2 WINDOW
SIGV2 APPROVE <resolved peer identity, colon-separated>
SIGV2 STATUS
SIGV2 GOLDEN
SIGV2 REVOKE <registered identity, including disconnected peer>
```

WINDOW expires after 90 seconds. APPROVE requires a currently encrypted,
bonded peer and locally verified identity. It stores CONNECT+SIGNAL_DEMO only
in `m5can_sigv2`, alternating CRC-checked, read-back-verified A/B generations.
The record binds resolved identity to a SHA-256 fingerprint of stored bond
material; missing/replaced bond or revoked record fails closed. No keys or
fingerprints are logged. PEERS prints identities only for explicit USB admin
inspection. Revoke never deletes the BLE bond. Re-registration starts a new
session; reconnect retains registration but never profile or lease.

The SmartRing console reports pending registration, HELLO, ACK, DEMO data and
recovery. It connects only to the bench M5Dial BLE address 48:ca:43:3a:11:f5
(base USB identity +1 on these two-universal-address ESP32-S3 builds); verify this on hardware before
changing it. Send `g` over its USB console for the firmware codec G01–G12 test.
No existing meter UI or acquisition pipeline is included in this lab image.

## Protocol and resource checks

- Fixed hex is in `tests/fixtures/signal_v2_v3.json` and thirteen G10 fragments.
- Python tests compile the actual firmware C++ codec/session with ASan/UBSan.
- Firmware GOLDEN runs the fixed wire codec tests, including G12 expected CRC
  rejection; this is distinct from complete on-air G01–G12 acceptance.
- Run `tools/signal_v2_ble_bench.py` from a Bluetooth-authorized Mac process.
  The owning macOS app must have NSBluetoothAlwaysUsageDescription; a bare
  Python child of an app without it is killed by TCC before a BLE connection.
- GATT: encrypted Control Write, Data Notify, Status Read+Indicate. Read is a
  generic safe state, never another peer's ACK. Logical packets use fragments
  on all lanes; MTU23/247, CRC and exact ordered reconstruction are checked.
- Eight cached Control results per session; content-changing reuse rejected;
  old evicted IDs yield RESULT_UNKNOWN. RENEW does not advance revision.
- SIMULATED_ADMISSION: <=8 entries fit a complete snapshot, periods >=200ms.
  Slower requested periods are honored independently. Faster requests with
  adequate max/deadline return RELAXED; explicit ACCEPT_PLAN is required.
  The relaxed lab ACK body is PlanId followed by the complete proposed profile.
- One shared synthetic source merges both live sessions' PID demand; expiry,
  disconnect, security loss and CCCD removal remove demand before source tick.
  Source reservations/acquisitions are synthetic counters, not physical CAN.
- Separate per-peer buffers and lane transfer IDs; one outstanding indication
  or notify fragment per peer; no mbuf allocation on RX callbacks. Failure or
  stalled indication times out after 2 seconds; queues cannot grow unbounded.
- Record origins are DEMO (0x02). G05 CAN RAW (0x01) is decoder-only; it is
  never emitted as a live observation. Snapshot preserves cached sample time
  and sequence. Heartbeat does not create observations. Invalid data is rejected.
- SmartRing requires matching boot/session/revision/source/mode, DEMO origin,
  complete snapshot and matching ResumeSequence. Resync <=5s, <=2 attempts.

## Interpretation requiring PM review

The frozen v3 text supplies complete Golden bytes but does not enumerate every
HELLO/24-byte subscription/RELAXED/Snapshot field or OriginFlags value. This
prototype preserves all Golden bytes and interprets entry byte 6 as priority,
byte 7 reserved, SourceGeneration at 12; Snapshot prefix as ResumeSequence,
flags=3, revision, count/reserved. DEMO=0x02 and the RELAXED body above require
explicit cross-client review before interoperability/production approval.
No minor/major bump or new production wire claim is made by this Draft.

## Acceptance and restoration

Record host and hardware results separately, including actual negotiated MTU,
first registration, automatic reconnect, revocation, both Central identities,
CCCD/security changes, indication stalls, shared acquisition counts, minimum
internal heap/largest block/stacks, and an 8-hour two-Central soak. A build,
boot-time Golden pass or short DEMO exchange does not satisfy these gates.

Restore only from the full-bank backup whose on-chip hash passed. Rediscover
the board, verify file SHA against its local manifest, and use the same current
Espressif OpenOCD with the exact adapter serial. `program_esp <full-backup.bin>
0 verify reset exit` writes the saved image/layout/settings without a whole-chip
erase; verify full-bank hash and original boot. Restoring NVS replaces lab-added
registrations/bonds with the captured baseline. Never use a failed old-tool
read as the restoration source. Full backups contain private bond/settings data
and must remain local, outside Git/PR artifacts.

### JTAG after-app verification workaround on this bench

The new app exposed a flasher-stub panic with both cores active. A CPU0-only
OpenOCD target plus holding CPU1 reset/stall during RAM-stub operations resolves
it. Use `set ESP_ONLYCPU 1` before esp32s3.cfg, then `reset halt; mww 0x600c0000 7`
(S3 SYSTEM_CORE_1_CONTROL_0, reset+clock+stall). Use `no_clock_boost` for programming;
verify app hash plus saved bootloader/partition/otadata sectors. Before normal
boot, restore this register to its reset default with `mww 0x600c0000 4`, then
`reset run`. This is a volatile reset control, not an eFuse/flash modification.
Never leave CPU1 stalled when evaluating the application. Reopen CDC only after
USB re-enumerates. This workaround is specific to the observed ESP32-S3 bench.

### Preferred ROM recovery if an application-triggered stub panic recurs

Use the chip's **volatile** forced-download bit, discovered from the pinned
ESP32-S3 SoC header and esptool's own hard-reset implementation. Via the matched
JTAG adapter: halt CPU0, write RTC_CNTL_OPTION1 (0x6000812c)=1, then read-modify-write
RTC_CNTL_OPTIONS0 (0x60008000) with bit31 set. This resets into ROM without any
flash/eFuse write. esptool 5.6.0 `--before no-reset` then connects and identifies
chip/MAC/JEDEC flash. `verify-flash` checks app/boot/partition/otadata independently
of the application. Its `--after hard-reset` explicitly clears forced download
and returns to normal boot. This proved reliable on both specified boards.
For future app updates prefer this ROM path after a verified full backup and
app-only gate; it avoids program_esp's internal reset undoing CPU1 isolation.
