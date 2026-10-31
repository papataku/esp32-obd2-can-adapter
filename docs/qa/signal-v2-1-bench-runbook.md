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
recovery. **No real BLE target identity is shipped or checked into Git.**
Before compiling on the operator's Mac, copy
`test_clients/smartring_signal_v2/signal_v2_target_local.h.example`
to the adjacent `signal_v2_target_local.h` (gitignored). Replace its
`SIGNAL_V2_TARGET_ADDRESS` placeholder with the target personally
verified through USB-only `SIGV2 PEERS` / local board checks. A missing
configuration disables all automatic connection; it never falls back to the
first advertised device. Do NOT commit this local file or copy its
contents into issues, CI, runbooks, logs or screenshots. Remove stale
compile artifacts before provisioning a different target.

`SIGV2 PEERS` is an **explicit USB administrator-only disclosure** and now
uses the `#SIGNALV2_ADMIN_PRIVATE` prefix. Do not capture it in automatic
bench evidence. Ordinary `SIGV2 STATUS`, startup logs and runbook contain
no full BLE addresses, keys or key fingerprints. The actual identity
remains accessible only to the present operator to perform APPROVE/REVOKE.
Send `g` over its USB console for the firmware codec G01–G12 test.
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
- SIMULATED_ADMISSION: up to 16 entries are parsed and advertised in HELLO;
  Admission computes the COMPLETE snapshot payload (16B prefix plus every
  expected 44B record + raw sample bytes) and atomically REJECTs any plan
  exceeding 486B. Nine RPM records (448B snapshot) are accepted, ten RPM
  records (496B) are rejected, without partial profile commit. All periods
  remain >=200ms after any explicit RELAXED approval.
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

## Normative wire addendum for PM review

The full v3 text plus offset-precise normative addendum is now checked into
`docs/v2/m5can-v2-spec-v3.md` on this PR. The addendum spells out HELLO,
24B subscription entry, OriginFlags, RELAXED/ACCEPT_PLAN, Snapshot and
lease/paused behavior. It retains existing G01-G12 bytes. Independent
interoperability qualification remains pending PM approval.

## Remaining *hardware* acceptance (NOT demonstrated by CI)

- A second physical Central: concurrent connections, one disconnect/lease
  expiry/revocation without harming the other, slow peer, indication stall,
  CCCD loss, security change and re-registration.
- Physical ATT MTU23 and MTU247 sessions, with packet and fragment captures.
- An actual on-air G01-G12 replay and deliberate DATA-gap then HEARTBEAT
  fault to force SNAPSHOT, including gap after reconnect.
- Multiple simultaneous subscription records on a physical Central:
  this SmartRing minimal client intentionally requests and accepts only
  ONE record; the host M5CAN Session codec tests 9-record DATA/SNAPSHOT.
- Eight-hour dual-Central soak with mbuf pressure, lost indications,
  RSSI/interval telemetry, heap minima, largest free block and stack marks.
- Hardware NVS write interruption and restoration, plus every existing
  saved bond/log verification. No destructive test without separate approval.
- Post-fix reflash/boot verification is **NOT performed in this change**;
  PM will commission the next authorized USB-only reflashing separately.
- No real vehicle CAN, SOC/POWER LIVE or new-CAN-observation Hz testing.

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

### Clean CI package installation

The SmartRing platform's esptool URL conflicts with the registry package that
the M5Dial build already installed under the same tool name: CI failed before
compilation with missing `package-postinstall.py`. Pin the common official
registry package `platformio/tool-esptoolpy@2.40900.250804` (esptool 4.9.0) for
image generation in the Ring env. The board, Arduino 3.1.1, flash/PSRAM/layout
remain the reference settings. USB ROM recovery uses separately isolated
venv esptool 5.6.0; its version does not change the firmware toolchain.

## V2-1b: paired five-signal SmartRing PRODUCT display

This section is ONLY for branch `feat/m5can-v2-1b-demo5`, paired with
`papataku/TacoTacoMeter` branch `feat/smartring-v2-client`, env
`viewe_smartring_lvgl_signal_v2_lab`. M5Dial uses the unchanged
`m5dial_signal_v2_can_disabled_lab` env. 200ms period; 10Hz is V2-2.

The synthetic source now defaults **OFF at every boot**, regardless of the
legacy V1 DEMO NVS setting. On M5Dial USB serial, commands are:
```
SIGV2 DEMO STATUS
SIGV2 DEMO ON
SIGV2 DEMO OFF
```
Only M5Dial USB SerialProtocol accepts them; they are NOT BLE/NUS commands.
ON is volatile, not persisted, and never enables CAN. Each of RPM (010C),
SPEED (010D), WATER (0105), SOC (015B), POWER (019A) changes as a
deterministic, physically plausible **synthetic** raw OBD payload. All five
records have origin DEMO (02), quality VALID/VALID_ZERO (00/01), source and
mode generation 1. OFF clears cached values and sends NO_DATA (02), data
length 0 and sample sequence 0; never emits zero as an unavailable sample.
No unsolicited SNAPSHOT changes the frozen V2 wire contract.

Follow the full cross-repository provisioning, enrollment, app-only image
verification, UI and recovery checklist in
`papataku/TacoTacoMeter/docs/qa/signal-v2-1b-product-ring-runbook.md`.
Use a privately verified gitignored target on the SmartRing operator Mac.
Do NOT flash, erase, or connect to a vehicle without the PM's separate
physical authorization. Check both exact firmware hashes and measure the
display quality states on the real SmartRing. This runbook/CI never proves
actual 5Hz/10Hz fresh ECU observation or SOC/POWER LIVE eligibility.
