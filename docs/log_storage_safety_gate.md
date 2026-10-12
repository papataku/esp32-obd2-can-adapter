# Read-only logging storage preflight / integration gate

## Why this is a standalone Draft PR

PR #25 introduces CAN first-probe/failure original-RAW NVS sidecars.
PR #26 instruments live NimBLE disconnects. PR #27 adds persistent BLE
error history and failure-category summaries. They have different
development owners and are **not yet combined**. Do not deploy a combined
logger without checking the actual ESP32 NVS partition.

This PR introduces **no additional persistent writes** and deliberately
does not modify the partition table, perform any NVS migration, erase
settings, change CAN TX policy, alter the BLE session model, or merge the
other PRs.

## Operator action (only while stationary, without active clients)

After flashing a separately approved diagnostic build, and only from a
safe/idle USB console:

```text
LOGCAP
```

Output groups:

- `#LOGCAP,PARTITION`: actual `nvs` label, flash offset, partition size
  and encryption flag, read from the live ESP-IDF partition table.
- `#LOGCAP,STATS`: `nvs_get_stats` used, free, available, total entries
  and namespaces for that NVS partition.
- `#LOGCAP,MODEL`: compiled size of the legacy DriveLog records (8 boot
  slots), coordinator sidecars, BLE error journal (4 slots in PR #27), and
  **raw-array payload-only minimum** projected for 8 × (initial group,
  first post-group failure) records from PR #25.
- `#LOGCAP,VERDICT`: `INSUFFICIENT` if that projected lower bound
  alone exceeds the entire partition; otherwise `UNPROVEN`. In all
  cases `write_approved=NO`, `method=READ_ONLY`.

If the partition or its NVS statistics are unavailable, the command prints
`decision=BLOCK`, never assumes a default address or silently initializes
a missing NVS partition. If CAN query or BLE clients are active, it returns
`ACTIVE_TRAFFIC` and does not query the NVS statistics.

**The lower bound is not a storage allocator**. It excludes NVS key/namespace
metadata, BLOB chunk overhead, fragmentation, reserved pages and
copy-on-update capacity. `nvs_get_stats` describes current NVS usage but
cannot guarantee the next big BLOB write succeeds. The command is a
*screening/measurement* instrument, never authorization to start writing.

## Integration stop conditions

1. Preserve all existing settings and old `DriveLog` NVS records. Before
   a logging storage migration, export the current `DRIVELOG`, `BLELOG`
   and `LOGCAP` output, inspect the partition table on the exact target,
   and back up flash according to the existing build/board workflow.
2. If `LOGCAP` is `INSUFFICIENT` or cannot read the partition/stats, **do
   not enable** the combined NVS logger. Create an explicitly approved,
   non-destructive partition/layout migration plan (dedicated append-only
   log store with CRC, bounded rotation and power-loss recovery) or reduce
   captured data with quantified retention guarantees.
3. If `LOGCAP` is `UNPROVEN`, the operation remains blocked pending a
   hardware test matrix measuring total NVS entries and actual BLOB update
   success with trip + BLE + full CAN sidecars over many boot cycles.
4. A full hardware power-cycle test, simulated sudden power-loss recovery,
   CAN/BLE latency/stack/heap profiling and data-provenance comparison are
   required before merging PR #25, #26 or #27 together.
5. Never auto-clear NVS, change ESP32 partition tables or rewrite a
   programmed device solely because diagnostic history is full.

## Test gates

Host test `tests/test_log_storage_probe.cpp` asserts exact logical
sizeof()-derived cost components and verifies that calling the host
`LOGCAP` probe performs **zero** Preferences writes and leaves existing
settings bytes unchanged. CI also builds all six PlatformIO profiles.
Native ESP32 read-only APIs are verified by compilation, not by an
inaccessible simulated partition. Real size and NVS stats need a physical
device.

## Sources

Espressif ESP-IDF NVS documentation: NVS BLOBs use 32-byte entries with
per-chunk overhead and updates require extra free space; overall available
entry count is not sufficient proof of space for a fragmented BLOB.
https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/nvs_flash.html
