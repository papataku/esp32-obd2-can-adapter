#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <Preferences.h>

#include "log_storage_probe.h"
#include "drive_log.h"
#include "ble_evidence_journal.h"

using namespace m5can;
int main() {
  const LogStorageEstimate e=estimateLogStorage();
  assert(e.legacy_trip_bytes==
      static_cast<uint64_t>(DriveLog::kSlots)*sizeof(DriveLog::Record));
  assert(e.legacy_coordinator_bytes==
      static_cast<uint64_t>(DriveLog::kSlots)*
      sizeof(DriveLog::CoordinatorRecord));
  assert(e.dual_ble_bytes==
      static_cast<uint64_t>(BleEvidenceJournal::kBootSlots)*
      sizeof(BleEvidenceJournal::Record));
  assert(e.grouped_raw_min_bytes==
      2ULL*DriveLog::kSlots*kDiagnosticMaxRawFrames*
      (sizeof(DiagnosticRawFrame)+sizeof(uint32_t)));
  assert(e.projected_min_bytes==
      e.legacy_trip_bytes+e.legacy_coordinator_bytes+
      e.dual_ble_bytes+e.grouped_raw_min_bytes);
  assert(e.projected_min_bytes > 0);

  // The host code intentionally cannot access physical partitions. The
  // diagnostic must not attempt writes, erase a namespace, or guess a size.
  Preferences::storage["existing/critical_settings"]={1,2,3,4,5};
  const auto existing=Preferences::storage;
  const auto writes=Preferences::writes;
  SerialStub::capture=true;SerialStub::lines.clear();
  reportLogStorageCapacity();
  assert(SerialStub::lines.find(
      "#LOGCAP,UNAVAILABLE,requires=ESP32,method=READ_ONLY")!=
      std::string::npos);
  assert(Preferences::writes==writes);
  assert(Preferences::storage==existing);
  SerialStub::capture=false;SerialStub::lines.clear();
  std::cout<<"PASS LOGCAP sizing lower bound and read-only host behavior: "
           <<e.projected_min_bytes<<" bytes logical payload minimum\n";
}
