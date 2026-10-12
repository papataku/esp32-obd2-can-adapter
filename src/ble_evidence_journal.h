#pragma once

#include <Arduino.h>
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
#include "ble_elm_transport.h"
#include "can_monitor.h"
#include "diagnostic_error_catalog.h"

namespace m5can {

// Host-queue events are copied by the low-priority Arduino loop, NEVER from
// NimBLE callbacks. Independent NVS namespace/key names from DriveLog v2
// and the experimental CAN probe/failure sidecars in PR #25.
class BleEvidenceJournal {
 public:
  static constexpr uint32_t kMagic=0x4D35424CU; // M5BL
  static constexpr uint16_t kVersion=1;
  static constexpr unsigned kBootSlots=4;
  static constexpr unsigned kEvents=12;
  static constexpr unsigned kFaultBuckets=48;

  struct Entry {
    uint32_t seq=0,ms=0,gen=0;
    uint16_t conn=0,peer_suffix=0,mtu=0,interval=0,latency=0,supervision=0;
    int32_t reason=0;
    uint8_t kind=0,reason_known=0;
    uint16_t reserved=0;
    uint32_t retries=0,failed=0;
    uint16_t code=0,reserved2=0;
    uint32_t detail=0;
    uint64_t rx_bytes=0,tx_bytes=0;
  };
  struct FaultSummary {
    uint16_t code=0,reserved=0;
    uint32_t count=0,first_ms=0,last_ms=0,first_detail=0,last_detail=0;
  };
  struct Record {
    uint32_t magic=kMagic;
    uint16_t version=kVersion,bytes=sizeof(Record);
    uint32_t crc=0,boot=0,saves=0,total=0,evicted=0,queue_drops=0;
    uint32_t first_seq=0,last_seq=0,seq_gaps=0;
    uint32_t unknown_faults=0,storage_errors=0;
    uint32_t can_result_status[10]{}; // owner-observed, includes USB requests
    uint8_t count=0,overflow=0;
    uint16_t reserved=0;
    char build[40]{};
    Entry events[kEvents]{};
    FaultSummary faults[kFaultBuckets]{};
  };
  static_assert(sizeof(Record)<=2304,"BLE evidence NVS record exceeds budget");
  void begin(uint32_t boot,const char* build);
  void append(const BleDiagnosticEvent& event,uint32_t queue_drops);
  void observeQueueDrops(uint32_t drops);
  // Snapshot monotonic driver/transport counters. Includes failures that
  // happen on CPU0 independently of BLE requests.
  void observeHealth(const CanStats& can,const BleStats& ble);
  void appendError(DiagnosticError code,uint32_t detail=0,
                   uint32_t timestamp_ms=0);
  bool poll(CanMonitor& can);
  bool saved() const {return !dirty_ && persisted_;}
  // USB retrieval intentionally refuses active peers/CAN reads. A long
  // serial dump must not stall ongoing diagnostic acquisition.
  void index(CanMonitor& can,BleElmTransport& ble);
  void dump(CanMonitor& can,BleElmTransport& ble,uint32_t boot=0);
  const Record& snapshot() const {return record_;}
 private:
  static uint32_t checksum(const Record& data);
  static bool load(unsigned slot,Record& data);
  static bool canExport(CanMonitor& can,BleElmTransport& ble);
  static void print(const Record& data,bool persisted);
  void recordFault(uint16_t code,uint32_t detail,uint32_t at_ms,
                   uint32_t occurrences=1);
  Record record_{},scratch_{};
  bool started_=false,dirty_=false,persisted_=false,urgent_=false;
  uint32_t last_attempt_ms_=0,last_success_ms_=0;
  CanStats last_can_{};
  BleStats last_ble_{};
  bool health_initialized_=false;
};
BleEvidenceJournal& bleEvidenceJournal();

} // namespace m5can
#endif
