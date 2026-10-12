#pragma once
#include <Arduino.h>
#include <cstdint>

namespace m5can {
// All figures are on-flash logical VALUE payloads. NVS uses additional
// entries, metadata, free pages, and copy-on-update space, so this model
// is strictly a LOWER BOUND, never a proof of write capacity.
struct LogStorageEstimate {
  uint64_t legacy_trip_bytes=0;
  uint64_t legacy_coordinator_bytes=0;
  uint64_t dual_ble_bytes=0;
  uint64_t grouped_raw_min_bytes=0;
  uint64_t projected_min_bytes=0;
};
LogStorageEstimate estimateLogStorage();
void reportLogStorageCapacity();
}
