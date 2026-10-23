# Existing single-app M5CAN vehicle trial (app-only)

**Preparation profile; NOT vehicle-qualified. NEVER use on OTA-layout devices.**

Environment: **m5dial_existing_singleapp_vehicle_trial**

- Extends m5dial_dual_ble_multipid_mtu_bench: max 2 BLE peers, ordinary adaptive multi-PID requests and **V1 CAN RAW** journal, MTU-limited ASCII notification (up to 244 bytes; each peer uses negotiated MTU).
- Real TWAI controller, normal CAN request/lease logic, initial Listen-Only. No CAN algorithm or TX policy changes.
- DEMO AUTO locked OFF in RAM, regardless of any historic NVS setting. It does **not** overwrite NVS; trial-mode toggle is disabled. All other envs unchanged.
- **No V2 GATT, no V2 bulk**, no demo fault fixture, no CAN dry-run and no RF/connection-interval change.
- Partner candidates, independently reviewed: Ring **viewe_smartring_lvgl_m5can_mtu_bench** (normal automatic BLE), T4 **lilygo_t4s3_lvgl_te_80mhz_m5can_bench** (BLE + 80MHz TE, V2 disabled). Existing installed diagnostic images are NOT those candidates.

## Explicit local build gate

Required local environment variables (the approved identity is private):

    M5CAN_VEHICLE_TRIAL_APPROVED_PEER_ID48=<approved private peer identity>
    M5CAN_VEHICLE_TRIAL_INSTALLED_PARTITIONS_SHA256=7891dd9f69f2da4a49b4ed63ec4bd1651f9258d0f0787249e3f7bae65591645f
    M5CAN_VEHICLE_TRIAL_APP_ONLY_ACK=APP_ONLY_NO_ERASE_KEEP_NONAPP

Run:

    pio run -e m5dial_existing_singleapp_vehicle_trial

The gate checks exact opt-in environment, non-destructive build ack, a private
identity's **format**, intended CSV and pinned archived partition digest.
It performs **no device probing, erase, flash or migration**. The identity
is not embedded, logged or enforced at runtime in V1; this is a *build-only
provenance gate*, not a BLE security allowlist.

The original single-app CSV says migration is needed for prior OTA layouts.
That warning remains true for non-migrated boards. This device was already
migrated and has an installed-partitions.bin archive with SHA256:

    7891dd9f69f2da4a49b4ed63ec4bd1651f9258d0f0787249e3f7bae65591645f

After compilation check firmware size <= 0x200000; factory region starts
0x10000. The built partitions.bin must hash to the archive above. Record
clean commit SHA, bin/ELF SHA256, and baseline stack comparison. This does not
itself prove the live device partition is unchanged.

## Physical app-only preparation/STOP

1. Read-only verify physical USB target, 8 MiB flash, partition bytes/offset
   and current installed manifest immediately before deployment. Any mismatch
   is STOP: **do not erase, repartition, format or try an OTA recovery**.
2. Preserve all non-app ranges including NVS, otadata, CANLOG, SPIFFS,
   coredump and evidence records A2/104 files/2part. Preserve all SD/RAW.
3. Only write the exact local candidate firmware.bin into **app at 0x10000**.
   Never run an uninspected generic PlatformIO upload that might include
   partition table/bootloader/otadata. One planned reboot expected.
4. Verify app read-back byte/hash matches candidate and **all non-app MD5**
   are unchanged. Any mismatch = STOP; no automatic retry or erase.
5. Inspect natural USB #BOOT git SHA, #DEMO setting=OFF active=NO, #CANLOG
   evidence status and BLE peers <=2. Missing state or spontaneous TX/reset
   is STOP. No claims of car CAN or real end-to-end logging success.
6. Vehicle gate begins with passive Listen-Only CAN after separate USB
   no-CAN smoke; diagnostic TX remains disabled until the known read-only
   OBD test gate, using existing time-limited lease safety.

The existing Ring/M5 issue #131 HCI 574 is **not solved**. A successful
single connection/boot does not qualify vehicle readiness. Build CI uses
a synthetic identity then **deletes its image**; never flash a CI binary.
