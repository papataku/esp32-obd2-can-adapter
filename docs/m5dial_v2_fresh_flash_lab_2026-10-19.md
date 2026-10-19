# M5Dial 8 MiB fresh-install V2 RAW/SD lab — destructive install approved

**Scope:** Operator explicitly approved removal of existing M5Dial internal Flash
data on 2026-10-19. This covers the *M5Dial only*: do not erase the T4-S3
SD card, iPad SQLite, the Mac or any car ECU. No flash or wipe is carried out
by this repository/CI, and no vehicle should be attached during preparation.

**Exact source:** check [Adapter PR #50](https://github.com/papataku/esp32-obd2-can-adapter/pull/50)
HEAD before build. Paired T4-S3:
[Meter PR #125](https://github.com/papataku/TacoTacoMeter/pull/125),
`t4s3_m5can_v2_authenticated_lab`. iPad Analyzer PR #21 has been merged
to main but is a separate ELM/SQLite client, not the V2 binary RAW consumer.

## Why this fresh install

M5Dial is ESP32-S3FN8 with **8 MiB** internal flash (M5Stack hardware spec).
The previous default two-OTA-slot layout has no `canlog` partition. The
fresh-install lab reuses the reviewed *single factory app* layout:

| Flash partition | Offset | Length |
| --- | --- | --- |
| NVS | 0x009000 | 0x005000 |
| OTA bookkeeping (left unused) | 0x00E000 | 0x002000 |
| Single factory app (no OTA) | 0x010000 | 0x200000 (2 MiB) |
| CANLOG custom unencrypted data subtype 0x40 | 0x210000 | 0x460000 (4.375 MiB) |
| SPIFFS/evidence address (empty after erase) | 0x670000 | 0x180000 (1.5 MiB) |
| coredump | 0x7F0000 | 0x010000 |

The CANLOG journal owns two 4 KiB sectors and uses append-only 4 KiB committed
blocks; effective raw data budget is smaller than 4.375 MiB.
It does not automatically reclaim. A second recording after a completed archive
requires a separate, explicit and tested reset/erase procedure. No automatic
reformat, log discard or partition migration.

## Compile-only gate (safe to perform without hardware)

```sh
git checkout integration/m5can-safe-default-20261019
git rev-parse HEAD
python tools/check_single_app_canlog_partition.py \
  partitions/m5can_8mb_single_app_canlog_proposed.csv
# Replace the value with the explicitly approved T4-S3 BLE IDENTITY
# from the existing paired/bonded peer inventory (not an arbitrary MAC).
export M5CAN_LOG_V2_ALLOWED_PEER_ID48='0xAABBCCDDEEFF'
export M5CAN_V2_FRESH_FLASH_ACK=ERASE_ALL_8MB_OLD_DATA_REQUIRED
pio run -e m5dial_v2_fresh_flash_lab
python tools/check_single_app_canlog_partition.py \
  partitions/m5can_8mb_single_app_canlog_proposed.csv \
  --firmware-bin .pio/build/m5dial_v2_fresh_flash_lab/firmware.bin
shasum -a 256 .pio/build/m5dial_v2_fresh_flash_lab/{firmware.bin,firmware.elf,partitions.bin}
```

For provisioning, the *opt-in lab T4-S3 firmware* now prints
`#V2_T4_LOCAL_ID=AA:BB:CC:DD:EE:FF,addr_type=...` to its own local USB
serial console (Meter PR #125). Verify that it is the exact BLE identity
resolved by M5Dial for the bonded client before using it as the value of
`M5CAN_LOG_V2_ALLOWED_PEER_ID48`. If the identity differs, stop rather
than allowing a generic newly paired peer. The local identifier must not be
posted to public PRs or issue comments.

The example `0xAABB...` is NOT an approved device identity. Build-time
validation checks the value's syntax but **cannot verify ownership**.
A changed/rotating BLE address, mismatched identity or absent bonding must
cause V2 control/START to fail closed. In the adapter code the allowlist
matches NimBLE's resolved `ble_gap_conn_desc.peer_id_addr.val[0..5]`.
Only the actual paired identity may be used, never the dummy CI identity
`0x010203040506`. Do not publish the full identity or generated image.

Without the exact consent variable the fresh-image build must fail. The
normal `m5dial` environment and default partition map are unchanged.

## Before a destructive USB install

1. Disconnect M5Dial's CAN-H/CAN-L/OBD, and use **USB power only**.
   Never include vehicle equipment in a Flash migration.
2. Confirm the connected USB device is the intended M5Dial (not T4-S3)
   and its chip model/actual flash size are ESP32-S3 / **8 MiB**:
   `pio device list`, and with the selected serial port
   `python -m esptool --chip esp32s3 --port "<M5DIAL_PORT>" flash_id`.
   Tool versions may spell the subcommand `flash-id`.
   **Stop if not 8 MiB.**
3. The existing firmware, OTA metadata, NVS bond/configuration, any old
   journal, coredump and stored evidence will all be lost. Re-pairing is
   necessary afterward. If any original data becomes valuable, make a
   read-only flash backup BEFORE erasing.
4. Confirm separately that the app binary fits its 2 MiB factory slot,
   the checked-in partition table matches the compiled `partitions.bin`,
   and the build image hash/source SHA was saved. Never use a CI test
   binary because it contains a synthetic allowlisted identity.
5. Flash encryption/Secure Boot protection must be evaluated separately.
   Never bypass security protections with `--force`.

## Controlled erase and upload (MANUAL — never run by CI or assistant)

Use the same local environment variables and replace the USB port manually.
`pio run -e m5dial_v2_fresh_flash_lab -t erase --upload-port "<M5DIAL_PORT>"`
is the operator's **destructive whole-chip erase**, not a benign OTA update.
Only after it succeeds, upload the *matching* factory app and partition image:

```sh
pio run -e m5dial_v2_fresh_flash_lab -t upload --upload-port "<M5DIAL_PORT>"
pio device monitor --port "<M5DIAL_PORT>" --baud 115200
```

If PlatformIO changes the selected serial port on reboot, rediscover the
port instead of flashing an arbitrary device. Do NOT use T4-S3 SD format.
If erase/upload fails, keep the car physically disconnected and recover by
USB bootloader flashing only after examining the error.

## First boot acceptance — vehicle disconnected

Required serial evidence:

- `#BUILD` or equivalent firmware source identity matches the local SHA.
- `#CANLOG,boot_ready=1,...blocks=0,foreign=0` after a fresh erase.
- `#V2,compiled=1,gatt_init=1,peer_allowlisted=1,canlog_ready=1,blocks=0,...`.
- A bonded approved T4 peer negotiates **actual** MTU 247 and discovers
  binary log service `b8596400-a2d7-4c26-9918-6a2d0b88fa00`.
- The unauthorized or unbonded peer cannot write V2 START. Ordinary
  ELM and 20B legacy remain available to separately paired clients.

No raw frames will appear unless a valid test fixture actually provides
them. Merely seeing V2 boot-ready does **not** prove radio transfer or CRC/ACK
performance. To test throughput, record a known synthetic offline CAN data
corpus on an approved isolated bench or retain a verifiable completed archive,
then transfer to T4-S3 SD and compare the exact SHA256. Never inject data into
a real vehicle to create a test fixture.

Submit M5Dial serial full log, T4-S3 log, firmware/partition SHA256 and
the read-only copied RAW/SD artifacts to the associated GitHub issue.
Stop on CANLOG foreign/error, unknown partition, unintended CAN TX, SD
error, missing END/CRC, unexpected reset, or V2 unauthorized access.
**All hardware tests remain pending until actually conducted.**
