# M5CAN 8MiB flash repartition proposal — DO NOT DEPLOY YET

> **2026-10-16 recommended alternative:** M5Dial's visible application sources contain no OTA update workflow; development firmware is loaded by wired USB. The user's priority is all-CAN trace retention, so the preferred *evaluation* design is a **single 2MiB factory application (no OTA)** and separate **4.375MiB raw CAN partition**, while the original **1.5MiB protected evidence area stays unchanged**. Total flash devoted to logs: **5.875MiB**, versus 3.875MiB for the dual-OTA proposal below. See [the single-app candidate](../partitions/m5can_8mb_single_app_canlog_proposed.csv) and `pio run -e m5dial_single_app_layout_eval`. Full 8MiB backup, old active OTA slot and bootloader/otadata selection inspection, a tested complete migration and safe USB recovery are **mandatory** before any device is flashed. Do not infer readiness from a compile pass. If the firmware binary does not fit 2MiB, measure its actual size and revise only after analysis. Regular `m5dial` remains on the deployed partition table for now.


This is a read-only design and compile feasibility exercise on Draft PR #32.
The normal m5dial environment retains the **original** partition table until
hardware backup, migration and power-loss acceptance are completed.

## Verified current flash map

The m5stack-stamps3 PlatformIO board definition selects default_8MB.csv and
8MiB flash. Historical adapter physical readout in
docs/integrated_logging_acceptance.md confirmed a 20KiB NVS partition and
an erased 0x180000-byte spiffs partition at 0x670000 later used as a protected
EvidenceStore; it has since received important logs.

| Partition | Current offset | Current size | Function |
|---|---:|---:|---|
| nvs | 0x9000 | 0x5000 | Settings, trip journal |
| otadata | 0xE000 | 0x2000 | OTA boot selection |
| app0 | 0x10000 | 0x330000 (3.1875MiB) | OTA app0 |
| app1 | 0x340000 | 0x330000 (3.1875MiB) | OTA app1 |
| spiffs | 0x670000 | 0x180000 (1.5MiB) | Existing owned evidence journal, NOT mounted |
| coredump | 0x7F0000 | 0x10000 | Crash evidence |

Thus 1.5MiB is NOT the total flash size. The two OTA app slots alone consume
6.375MiB even if the executable is much smaller. For raw full-CAN capture,
changing allocation is reasonable, but does not itself enlarge the current
EvidenceStore nor guarantee lossless capture for arbitrary session lengths.

## Proposed layout: preserve old journal in place

See partitions/m5can_8mb_dual_ota_canlog_proposed.csv.

| Partition | Proposed offset | Proposed size | Difference |
|---|---:|---:|---|
| nvs | 0x9000 | 0x5000 | Unchanged |
| otadata | 0xE000 | 0x2000 | Unchanged contents; verify before boot |
| app0 | 0x10000 | 0x200000 (2MiB) | Shrink only |
| app1 | 0x210000 | 0x200000 (2MiB) | Move from 0x340000 |
| canlog | 0x410000 | 0x260000 (2.375MiB) | New TYPE_DATA custom subtype 0x40, NOT a filesystem |
| spiffs | 0x670000 | 0x180000 (1.5MiB) | Keep prior 20-key EvidenceStore intact |
| coredump | 0x7F0000 | 0x10000 | Unchanged |

Log-dedicated flash total: 1.5 + 2.375 = **3.875MiB**.
The NEW continuous raw CAN journal should use the new canlog partition,
with its own ownership and CRC/commit-last. Existing EvidenceStore keeps its
header/sector count and protected keys unchanged, so old diagnostics stay
readable after *a correct* migration. Firmware must fit BOTH 2MiB OTA slots.
CPU/flash overhead and compression savings require separate measurements.

## Critical migration blockers

The previous app1 region was 0x340000..0x670000; the new canlog partition
0x410000..0x670000 overlaps it. Therefore **canlog is NOT blank on any
arbitrary in-service board** and must never be automatically formatted/erased.
The new app1 starts at 0x210000, overlapping the old app0 reservation,
so an OTA partition-table-only update can break the selected boot image or
overwrite active code. OTA metadata and app0/app1 binaries must be inspected
and migrated as one controlled release. Rolling back ONLY the app image or
ONLY the partition table is unsafe.

An approved operator migration requires all of:
1. Read and privately back up all physical 8MiB flash, partition table,
   bootloader, otadata, NVS and the old 0x670000 evidence with SHA256, plus
   USB DRIVELOG/BLELOG, before any writes. Verify original firmware hashes.
2. Check ESP physical flash size, hardware revision, current active OTA slot,
   actual firmware.bin/app header and 2MiB fit on both app slots, and exact
   partition table before making any sector changes.
3. Create independent tested migration/recovery steps for existing OTA
   contents, otadata, NVS and old evidence; preserve rollback artifacts and
   physical USB recovery. Never rely on automatic Arduino filesystem
   formatting or partition generation as migration.
4. Boot the test firmware with physical CAN TX disabled on a spare/bench
   board, with explicit approval to apply the new partition table and
   reflash both app slots. Verify boot sequence and both OTA slots,
   EvidenceStore read-only history and non-overlap before authorizing
   canlog ownership initialization.
5. Obtain explicit operator authorization for one-time erasure of the OLD
   app1 bytes now within canlog, with prior verified backup. Nonblank
   contents must fail closed until then; never erase by merely calling
   LOG_BEGIN. Test torn-owner, torn-block, CRC and electrical power loss.
6. Test full CAN throughput/latency, dual BLE, compression, SD export and
   worst-case flash erase under realistic loads. Only then consider making
   this the normal partition choice.

## Other allocation choices

- **Keep two OTA apps, 2.5MiB each:** produces a 2.875MiB total log
  allocation if the entire remaining 2.875MiB is reallocated. Still
  requires journal migration if moving spiffs; protecting the original
  1.5MiB in place would provide 1.375MiB extra canlog without moving it.
- **Remove the second OTA app:** app0 remains at 0x10000..0x340000,
  the former app1 becomes another 3.1875MiB log partition, the existing
  spiffs 1.5MiB stays at 0x670000, for 4.6875MiB total log space.
  This sacrifices OTA rollback and requires a dedicated USB recovery
  flow plus OTA metadata handling; not selected by default.

## Read-only validation

    python tools/check_canlog_partition.py partitions/m5can_8mb_dual_ota_canlog_proposed.csv
    pio run -e m5dial_flash_layout_eval
    python tools/check_canlog_partition.py partitions/m5can_8mb_dual_ota_canlog_proposed.csv --firmware-bin .pio/build/m5dial_flash_layout_eval/firmware.bin

The CI build uses the proposed partition CSV only for the explicit
m5dial_flash_layout_eval environment. It does not write the real device, and
passing this build is NOT authorization to migrate current flash contents.

In this design user-facing CAN logging and grouped PID features still compile
ON by default in the normal firmware; there is no feature OFF switch. Recording
is a deliberate START/STOP session, independent from build flags.
