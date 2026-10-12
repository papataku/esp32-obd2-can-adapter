#include "log_storage_probe.h"
#include "drive_log.h"
#include "evidence_store.h"
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
#include "ble_evidence_journal.h"
#endif

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_partition.h>
#include <nvs_flash.h>
#endif

namespace m5can {

LogStorageEstimate estimateLogStorage() {
  LogStorageEstimate out{};
  out.legacy_trip_bytes=
      static_cast<uint64_t>(DriveLog::kSlots)*sizeof(DriveLog::Record);
  out.legacy_coordinator_bytes=
      static_cast<uint64_t>(DriveLog::kSlots)*sizeof(DriveLog::CoordinatorRecord);
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  out.dual_ble_bytes=static_cast<uint64_t>(BleEvidenceJournal::kBootSlots)*
                     sizeof(BleEvidenceJournal::Record);
#endif
  // When PR #25 and PR #27 are combined, first probe and first subsequent
  // failure can each retain up to kDiagnosticMaxRawFrames accepted raw
  // frames and their 32-bit RX deltas for all eight boot slots. This is
  // the raw ARRAY CONTENT ONLY, deliberately excluding ProbeRecord headers,
  // CRC, NVS keys/metadata, fragmentation and update-copy space.
  out.grouped_raw_min_bytes=2ULL*DriveLog::kSlots*kDiagnosticMaxRawFrames*
                           (sizeof(DiagnosticRawFrame)+sizeof(uint32_t));
  out.projected_min_bytes=out.legacy_trip_bytes+
      out.legacy_coordinator_bytes+out.dual_ble_bytes+
      out.grouped_raw_min_bytes;
  return out;
}

void reportLogStorageCapacity() {
#if defined(ARDUINO_ARCH_ESP32)
  // READ-ONLY inspection: never nvs_open WRITE, nvs_set, commit, erase,
  // partition resize, or flash any binaries from this command.
  const esp_partition_t* partition=esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_NVS,"nvs");
  if(!partition) {
    Serial.println("#LOGCAP,ERR,NO_DEFAULT_NVS,decision=BLOCK");
    return;
  }
  nvs_stats_t stats{};
  const esp_err_t code=nvs_get_stats(partition->label,&stats);
  Serial.printf("#LOGCAP,PARTITION,label=%s,offset=%lu,size_bytes=%lu,encrypted=%u\n",
      partition->label,static_cast<unsigned long>(partition->address),
      static_cast<unsigned long>(partition->size),
      static_cast<unsigned>(partition->encrypted));
  if(code!=ESP_OK) {
    Serial.printf("#LOGCAP,ERR,NVS_STATS_FAILED,esp_err=%ld,decision=BLOCK\n",
                  static_cast<long>(code));
    return;
  }
  // This PlatformIO-pinned ESP-IDF uses the legacy nvs_stats_t without
  // available_entries. Do not infer allocatable blob space from free alone.
  Serial.printf("#LOGCAP,STATS,used=%lu,free=%lu,total=%lu,namespaces=%lu,blob_write_capacity=UNKNOWN\n",
      static_cast<unsigned long>(stats.used_entries),
      static_cast<unsigned long>(stats.free_entries),
      static_cast<unsigned long>(stats.total_entries),
      static_cast<unsigned long>(stats.namespace_count));

  const auto est=estimateLogStorage();
  Serial.printf("#LOGCAP,MODEL,legacy_trip=%llu,legacy_coord=%llu,ble=%llu,group_raw_min=%llu,payload_min=%llu\n",
      static_cast<unsigned long long>(est.legacy_trip_bytes),
      static_cast<unsigned long long>(est.legacy_coordinator_bytes),
      static_cast<unsigned long long>(est.dual_ble_bytes),
      static_cast<unsigned long long>(est.grouped_raw_min_bytes),
      static_cast<unsigned long long>(est.projected_min_bytes));

  const bool impossible=est.projected_min_bytes>partition->size;
  // Even if this minimum fits, available entry COUNT alone cannot prove
  // enough contiguous space or reserve for wear-levelled blob updates.
  Serial.printf("#LOGCAP,VERDICT,NVS_only_PR25_PR27=%s,bytes_shortfall=%llu,write_approved=NO,method=READ_ONLY\n",
      impossible?"INSUFFICIENT":"UNPROVEN",
      static_cast<unsigned long long>(impossible ?
          est.projected_min_bytes-partition->size : 0));
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  const auto& durable=evidenceStore().status();
  Serial.printf("#LOGCAP,DURABLE,ready=%u,foreign=%u,sectors=%lu,protected_keys=%lu,commits=%lu,errors=%lu,corrupt=%lu,uncommitted=%lu\n",
      durable.ready?1U:0U,durable.foreign?1U:0U,
      static_cast<unsigned long>(durable.sectors),
      static_cast<unsigned long>(EvidenceStore::kKeys),
      static_cast<unsigned long>(durable.commits),
      static_cast<unsigned long>(durable.errors),
      static_cast<unsigned long>(durable.corrupt),
      static_cast<unsigned long>(durable.uncommitted));
  Serial.printf("#LOGCAP,LAYOUT,legacy_NVS_payload=%llu,evidence_partition=spiffs,table_changed=NO,filesystem_mounted=NO,method=READ_ONLY\n",
      static_cast<unsigned long long>(est.legacy_trip_bytes+est.legacy_coordinator_bytes));
#endif

#else
  Serial.println("#LOGCAP,UNAVAILABLE,requires=ESP32,method=READ_ONLY");
#endif
}

} // namespace m5can
