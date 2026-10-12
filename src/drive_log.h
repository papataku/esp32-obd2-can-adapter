#pragma once
#include <Arduino.h>
#include "can_monitor.h"
#include "ble_elm_transport.h"

namespace m5can {
// Bounded post-drive journal; only Arduino loop/USB commands touch NVS.
// BLE worker only updates the protected RAM record.
class DriveLog {
 public:
  static constexpr uint32_t kMagic=0x4D354C47, kVersion=2, kSlots=8;
  enum class Kind:uint8_t { GroupEnabled=1, GroupFallback=2, GroupStopped=3,
                           Disconnect=4, LeaseDenied=5, CanFault=6 };
  struct Event {
    uint32_t uptime_ms=0, responder=0,transaction_us=0,first_response_us=0;
    uint16_t peer=0;
    uint8_t kind=0, status=0, responses=0, raw_count=0;
    DiagnosticRawFrame raw[2]{};
  };
  // The version-2 journal stores DiagnosticRawFrame in its Event ABI.
  // New RX metadata must never change the size of these legacy records.
  static_assert(sizeof(DiagnosticRawFrame)==16,
                "legacy NVS RAW size must not change");
  struct Record {
    uint32_t magic=kMagic,version=kVersion,size=sizeof(Record),sequence=0;
    uint32_t reset_reason=0,rom_reset_reason=0,uptime_ms=0,saves=0;
    char build[32]{};
    uint32_t grouped=0,individual=0,group_ok=0,fallback=0,stopped=0;
    uint32_t disconnects=0,rx_overflow=0,notify_failed=0,notify_retries=0;
    uint64_t group_time_us=0;
    uint32_t group_max_us=0;
    uint64_t can_rx=0,tx_attempts=0,tx_success=0,queries=0,timeouts=0;
    uint64_t query_ok=0,no_data=0,query_failed=0,drops=0;
    uint32_t bus_errors=0,missed=0,overrun=0,lease_ms=0;
    uint32_t can_stack_free=0,ble_stack_free=0,heap_min=0,largest_heap=0;
    uint16_t peers[2]{};
    uint8_t clients=0,demo=0,fault=0,multipid=0,event_count=0;
    uint32_t events_added=0;
    Event events[8]{}; // Last eight state/fault events, not per-query writes.
  };
  static_assert(sizeof(Event)==56,
                "legacy NVS event ABI must remain readable");
  // Sidecar preserves the version-2 boot journal ABI and old trip history.
  struct CoordinatorRecord {
    uint32_t magic=0x4D35434FU,version=1,sequence=0,uptime_ms=0;
    uint32_t sampled_ms=0,hits=0,misses=0,joins=0,groups=0,cross=0,singles=0;
    uint64_t owner_cache_hits=0;
  };
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
  // Separate, once-per-boot first grouped CAN probe evidence. Rotates with
  // the existing eight boot slots, independently of the last-8 event ring.
  // Never overwrite with later disconnects or recurring successful queries.
  struct ProbeRecord {
    uint32_t magic=0x4D355052U,version=1,size=sizeof(ProbeRecord),crc=0;
    uint32_t sequence=0,uptime_ms=0,responder=0;
    uint32_t transaction_us=0,first_response_us=0;
    uint8_t status=0,decision=0,reject_reason=0,responses=0;
    uint8_t raw_count=0,raw_saved=0,truncated=0,reserved=0;
    DiagnosticRawFrame raw[kDiagnosticMaxRawFrames]{};
    uint32_t rx_offset_us[kDiagnosticMaxRawFrames]{};
  };
  void recordFirstProbe(uint16_t peer,const DiagnosticResult& result,
                        uint32_t responder,uint8_t decision,uint8_t reject_reason);
  // If a learned ECU later fails, independently retain its FIRST failure.
  void recordFirstFailure(uint16_t peer,const DiagnosticResult& result,
                          uint32_t responder,uint8_t decision,uint8_t reject_reason);
  bool probeSaved() const;
#endif
  void coordinator(uint32_t hits,uint32_t misses,uint32_t joins,
                   uint32_t groups,uint32_t cross,uint32_t singles);
  void begin(uint32_t reset_reason,uint32_t rom_reset_reason=0);
  void submitted(const DiagnosticRequest& request);
  void outcome(Kind kind,uint16_t peer,const DiagnosticResult& result,
               uint32_t responder);
  void event(Kind kind,uint16_t peer);
  void resources(uint32_t can_stack,uint32_t ble_stack,uint32_t heap_min,uint32_t largest);
  void poll(CanMonitor& can,BleElmTransport& ble,bool demo);
  void dump(CanMonitor& can,BleElmTransport& ble,bool demo);
  Record snapshot() const;
 private:
  static bool load(unsigned slot,Record& record);
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
  static uint32_t probeChecksum(const ProbeRecord& record);
  static bool loadProbe(unsigned slot,ProbeRecord& record,
                        const char* prefix="probe");
  void populateEvidence(ProbeRecord& record,const DiagnosticResult& result,
                        uint32_t responder,uint8_t decision,uint8_t reason);
  ProbeRecord probe_{},failure_{};
  bool probe_present_=false,probe_dirty_=false,probe_saved_=false;
  bool failure_present_=false,failure_dirty_=false,failure_saved_=false;
  uint32_t probe_last_attempt_ms_=0;
#endif
  bool save(uint32_t now,bool forced=false,CanMonitor* can=nullptr);
  void addEvent(Kind kind,uint16_t peer,const DiagnosticResult* result,
                uint32_t responder);
  mutable portMUX_TYPE mux_=portMUX_INITIALIZER_UNLOCKED;
  Record record_{};
  CoordinatorRecord coordinator_{};
  bool coordinator_enabled_=false;
  bool enabled_=false,urgent_=false;
  uint32_t last_saved_ms_=0;
};
DriveLog& driveLog();
} // namespace m5can
