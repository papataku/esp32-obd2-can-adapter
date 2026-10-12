#include "ble_evidence_journal.h"
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
#include "evidence_preferences.h"
#include <cstdio>
#include <cstring>

namespace m5can {
BleEvidenceJournal& bleEvidenceJournal() {
  static BleEvidenceJournal journal;
  return journal;
}

uint32_t BleEvidenceJournal::checksum(const Record& record) {
  uint32_t hash=2166136261U;
  const auto* bytes=reinterpret_cast<const uint8_t*>(&record);
  for(size_t i=0;i<sizeof(record);++i)
    hash=(hash ^ bytes[i])*16777619U;
  return hash;
}
bool BleEvidenceJournal::load(unsigned slot,Record& target) {
  EvidencePreferences prefs;
  char key[12]{};
  std::snprintf(key,sizeof(key),"ble%u",slot);
  if(!prefs.begin("m5can_blelog",true))return false;
  const size_t bytes=prefs.getBytesLength(key);
  constexpr size_t legacy_bytes=sizeof(Record)-8; // v2 used build[40]
  if(bytes!=sizeof(Record) && bytes!=legacy_bytes){prefs.end();return false;}
  const bool present=prefs.getBytes(key,&target,bytes)==bytes;
  prefs.end();
  const bool legacy=target.version==2 && bytes==legacy_bytes;
  if(!present || target.magic!=kMagic ||
     (!legacy && target.version!=kVersion) || target.bytes!=bytes ||
     !target.boot || target.count>kEvents)return false;
  const uint32_t saved_crc=target.crc;target.crc=0;
  uint32_t hash=2166136261U;
  const auto* raw=reinterpret_cast<const uint8_t*>(&target);
  for(size_t i=0;i<bytes;++i)hash=(hash^raw[i])*16777619U;
  target.crc=saved_crc;
  if(hash!=saved_crc)return false;
  if(legacy) {
    // Read-only conversion after validating the ORIGINAL v2 bytes. Keep
    // schema/CRC provenance; never rewrite/delete an older NVS blob.
    const size_t tail=offsetof(Record,events);
    auto* data=reinterpret_cast<uint8_t*>(&target);
    std::memmove(data+tail,data+tail-8,sizeof(Record)-tail);
    std::memset(target.build+40,0,8);
  }
  return true;
}
void BleEvidenceJournal::begin(uint32_t boot,const char* build) {
  record_=Record{};
  scratch_=Record{};
  record_.boot=boot;
  if(build)std::snprintf(record_.build,sizeof(record_.build),"%s",build);
  started_=boot!=0;
  dirty_=persisted_=urgent_=false;
  last_attempt_ms_=last_success_ms_=0;
  last_can_=CanStats{};last_ble_=BleStats{};health_initialized_=false;
}
void BleEvidenceJournal::recordFault(uint16_t code,uint32_t detail,uint32_t at_ms,
                                     uint32_t occurrences) {
  if(!code || !started_)return;
  for(unsigned i=0;i<kFaultBuckets;++i) {
    auto& b=record_.faults[i];
    if(!b.code || b.code==code) {
      if(!b.code) {b.code=code;b.first_ms=at_ms;b.first_detail=detail;}
      b.count=UINT32_MAX-b.count<occurrences ?
          UINT32_MAX:b.count+occurrences;
      b.last_ms=at_ms;b.last_detail=detail;
      dirty_=urgent_=true;
      return;
    }
  }
  ++record_.unknown_faults; // Exhaustion is explicit
  dirty_=urgent_=true;
}
void BleEvidenceJournal::appendError(DiagnosticError code,uint32_t detail,
                                     uint32_t at_ms) {
  recordFault(static_cast<uint16_t>(code),detail,at_ms?at_ms:millis());
}
void BleEvidenceJournal::observeHealth(const CanStats& can,const BleStats& ble) {
  if(!started_)return;
  if(!health_initialized_) {
    last_can_=CanStats{};last_ble_=BleStats{};health_initialized_=true;
  }
  const uint32_t now=millis();
  auto recordDelta=[&](uint64_t newer,uint64_t old,DiagnosticError code) {
    if(newer>old)
      recordFault(static_cast<uint16_t>(code),
          static_cast<uint32_t>(newer-old>UINT32_MAX?UINT32_MAX:newer-old),
          now,static_cast<uint32_t>(newer-old>UINT32_MAX?UINT32_MAX:newer-old));
  };
  recordDelta(can.query_timeout,last_can_.query_timeout,DiagnosticError::CanQueryTimeout);
  recordDelta(can.query_failed,last_can_.query_failed,DiagnosticError::CanFailedCounter);
  recordDelta(can.app_queue_drops,last_can_.app_queue_drops,DiagnosticError::CanRxQueueDrop);
  recordDelta(can.driver_rx_missed,last_can_.driver_rx_missed,DiagnosticError::CanDriverMissed);
  recordDelta(can.driver_rx_overrun,last_can_.driver_rx_overrun,DiagnosticError::CanDriverOverrun);
  recordDelta(can.driver_bus_error,last_can_.driver_bus_error,DiagnosticError::CanBusErrorCounter);
  recordDelta(can.driver_arb_lost,last_can_.driver_arb_lost,DiagnosticError::CanArbitrationLoss);
  recordDelta(ble.rx_overflow,last_ble_.rx_overflow,DiagnosticError::BleRxOverflowCounter);
  recordDelta(ble.notify_failures,last_ble_.notify_failures,DiagnosticError::BleNotifyFailureCounter);
  recordDelta(ble.notify_retries,last_ble_.notify_retries,DiagnosticError::BleNotifyRetryCounter);
  recordDelta(ble.session_rejections,last_ble_.session_rejections,DiagnosticError::BleSessionRejectCounter);
  if(can.nrc_count>last_can_.nrc_count) {
    const uint32_t delta=can.nrc_count-last_can_.nrc_count;
    const auto nrc=static_cast<uint16_t>(DiagnosticError::CanNegativeResponse);
    const auto context=static_cast<uint16_t>(DiagnosticError::OwnerRequestContext);
    if(!last_can_.nrc_count) {
      recordFault(nrc,can.first_nrc_detail,can.first_nrc_ms);
      recordFault(context,can.first_nrc_request,can.first_nrc_ms);
      if(delta>1) {
        recordFault(nrc,can.last_nrc_detail,can.last_nrc_ms,delta-1);
        recordFault(context,can.last_nrc_request,can.last_nrc_ms,delta-1);
      }
    } else {
      recordFault(nrc,can.last_nrc_detail,can.last_nrc_ms,delta);
      recordFault(context,can.last_nrc_request,can.last_nrc_ms,delta);
    }
  } else if(can.query_failed>last_can_.query_failed || can.query_no_data>last_can_.query_no_data) {
    recordFault(static_cast<uint16_t>(DiagnosticError::OwnerRequestContext),can.last_failed_request,can.last_failed_ms);
  }
  // Exact CAN owner outcome histogram, including requests made over USB
  // or results lost after a BLE peer disconnect.
  for(unsigned i=0;i<10;++i) {
    if(can.result_status_count[i]!=record_.can_result_status[i]) {
      if(i>0 && can.result_status_count[i]>record_.can_result_status[i])
        urgent_=true;
      record_.can_result_status[i]=can.result_status_count[i];
      dirty_=true;
    }
  }
  last_can_=can;last_ble_=ble;
}
void BleEvidenceJournal::observeQueueDrops(uint32_t drops) {
  if(!started_ || drops==record_.queue_drops)return;
  if(drops>record_.queue_drops)
    recordFault(static_cast<uint16_t>(DiagnosticError::BleDiagnosticRingOverflow),
                drops-record_.queue_drops,millis());
  record_.queue_drops=drops;
  dirty_=urgent_=true;
}
void BleEvidenceJournal::append(const BleDiagnosticEvent& event,
                                uint32_t queue_drops) {
  if(!started_)return;
  observeQueueDrops(queue_drops);
  if(record_.total && event.sequence>record_.last_seq &&
     event.sequence-record_.last_seq>1)
    record_.seq_gaps+=event.sequence-record_.last_seq-1;
  if(!record_.total)record_.first_seq=event.sequence;
  record_.last_seq=event.sequence;
  ++record_.total;
  if(record_.count==kEvents) {
    for(unsigned i=1;i<kEvents;++i)record_.events[i-1]=record_.events[i];
    ++record_.evicted;
    record_.overflow=1;
  } else ++record_.count;
  Entry& out=record_.events[record_.count-1];
  out=Entry{};
  out.seq=event.sequence;out.ms=event.uptime_ms;out.gen=event.generation;
  out.conn=event.connection_id;out.peer_suffix=event.address_suffix;
  out.mtu=event.mtu;out.interval=event.interval;out.latency=event.latency;
  out.supervision=event.supervision;out.reason=event.reason;
  out.reason_known=event.reason_known?1:0;
  out.kind=static_cast<uint8_t>(event.kind);
  out.retries=event.retries;out.failed=event.failed;
  out.code=event.error_code;out.detail=event.error_detail;
  out.rx_bytes=event.rx_bytes;out.tx_bytes=event.tx_bytes;
  if(event.kind==BleDiagnosticEvent::Kind::Disconnect) {
    PeerDisconnect* chosen=nullptr;
    for(auto& summary:record_.disconnect_peers) {
      if(summary.count &&
         ((event.address_suffix!=0 &&
           summary.address_suffix==event.address_suffix) ||
          (event.address_suffix==0 && summary.address_suffix==0 &&
           summary.conn==event.connection_id &&
           summary.generation==event.generation))) {
        chosen=&summary;
        break;
      }
    }
    if(!chosen)
      for(auto& summary:record_.disconnect_peers)if(!summary.count) {
        chosen=&summary;
        chosen->address_suffix=event.address_suffix;
        chosen->first_ms=event.uptime_ms;
        chosen->first_reason=event.reason_known?event.reason:INT32_MIN;
        break;
      }
    if(chosen) {
      chosen->conn=event.connection_id;
      chosen->generation=event.generation;
      if(chosen->count!=UINT32_MAX)++chosen->count;
      chosen->last_ms=event.uptime_ms;
      chosen->last_reason=event.reason_known?event.reason:INT32_MIN;
    } else if(record_.peer_summary_overflow!=UINT32_MAX) {
      ++record_.peer_summary_overflow;
    }
  }
  if(event.kind==BleDiagnosticEvent::Kind::Disconnect)
    recordFault(static_cast<uint16_t>(DiagnosticError::BleDisconnected),
                event.reason_known?static_cast<uint32_t>(event.reason):UINT32_MAX,
                event.uptime_ms);
  if(event.kind==BleDiagnosticEvent::Kind::Error || event.error_code)
    recordFault(event.error_code,event.error_detail,event.uptime_ms);
  dirty_=true;
  if(event.kind!=BleDiagnosticEvent::Kind::Connect)urgent_=true;
}
bool BleEvidenceJournal::poll(CanMonitor& can) {
  if(!started_ || !dirty_)return false;
  const uint32_t now=millis();
  // Never flash on every notification or event. A disconnect is a high
  // priority checkpoint, but repeated flapping still has a hard 5s floor.
  if(now<1000 ||
     (last_attempt_ms_ && now-last_attempt_ms_<2000) ||
     (last_success_ms_ && now-last_success_ms_<
         (urgent_?5000U:30000U)))return false;
  if(!can.tryBeginLogCheckpoint())return false;
  last_attempt_ms_=now;
  scratch_=record_;
  scratch_.saves=record_.saves+1;
  scratch_.crc=0;
  scratch_.crc=checksum(scratch_);
  EvidencePreferences prefs;
  char key[12]{};
  std::snprintf(key,sizeof(key),"ble%u",record_.boot%kBootSlots);
  bool wrote=false;
  if(prefs.begin("m5can_blelog",false)) {
    wrote=prefs.putBytes(key,&scratch_,sizeof(scratch_))==sizeof(scratch_);
    prefs.end();
  }
  const uint32_t expected_saves=scratch_.saves;
  // Reuse the preallocated workspace for NVS readback verification.
  // Never put a multi-KiB Record on the Arduino/UI task stack.
  const bool ok=wrote && load(record_.boot%kBootSlots,scratch_) &&
      scratch_.boot==record_.boot && scratch_.saves==expected_saves;
  can.endLogCheckpoint();
  if(ok) {
    record_.saves=scratch_.saves;
    dirty_=urgent_=false;
    persisted_=true;
    last_success_ms_=now;
  } else {
    if(record_.storage_errors!=UINT32_MAX)++record_.storage_errors;
    appendError(DiagnosticError::JournalStorageFailure,record_.storage_errors);
    persisted_=false;
    Serial.println("#BLELOG,storage_error");
  }
  return ok;
}
bool BleEvidenceJournal::canExport(CanMonitor& can,BleElmTransport& ble) {
  if(can.queryPending() || ble.connectedClientCount()) {
    Serial.println("#BLELOG,ERR,ACTIVE_TRAFFIC,disconnect_peers_before_dump");
    return false;
  }
  return true;
}
void BleEvidenceJournal::print(const Record& r,bool persisted) {
  Serial.printf("#BLELOG,BOOT,boot=%lu,state=%s,version=%u,build=%s,total=%lu,kept=%u,evicted=%lu,queue_drops=%lu,seq_gaps=%lu,first_seq=%lu,last_seq=%lu,saves=%lu\n",
      static_cast<unsigned long>(r.boot),persisted?"persisted":"RAM_ONLY",
      static_cast<unsigned>(r.version),r.build,
      static_cast<unsigned long>(r.total),r.count,
      static_cast<unsigned long>(r.evicted),
      static_cast<unsigned long>(r.queue_drops),
      static_cast<unsigned long>(r.seq_gaps),
      static_cast<unsigned long>(r.first_seq),
      static_cast<unsigned long>(r.last_seq),
      static_cast<unsigned long>(r.saves));
  Serial.printf("#BLELOG,INTEGRITY,boot=%lu,storage_errors=%lu,unclassified=%lu,overflow=%u\n",
      static_cast<unsigned long>(r.boot),
      static_cast<unsigned long>(r.storage_errors),
      static_cast<unsigned long>(r.unknown_faults),r.overflow);
  for(unsigned status=0;status<10;++status)if(r.can_result_status[status]) {
    Serial.printf("#BLELOG,CAN_STATUS,boot=%lu,status=%u,code=%u,name=%s,count=%lu\n",
        static_cast<unsigned long>(r.boot),status,
        static_cast<unsigned>(diagnosticStatusErrorCode(static_cast<DiagnosticStatus>(status))),
        diagnosticErrorName(diagnosticStatusErrorCode(static_cast<DiagnosticStatus>(status))),
        static_cast<unsigned long>(r.can_result_status[status]));
  }
  Serial.printf("#BLELOG,PEER_SUMMARY,boot=%lu,overflow=%lu\n",
      static_cast<unsigned long>(r.boot),
      static_cast<unsigned long>(r.peer_summary_overflow));
  for(const auto& peer:r.disconnect_peers) if(peer.count)
    Serial.printf("#BLELOG,PEER_DISCONNECT,boot=%lu,peer_suffix=%04X,id=%u,gen=%lu,count=%lu,first_ms=%lu,last_ms=%lu,first_reason=%ld,last_reason=%ld\n",
        static_cast<unsigned long>(r.boot),peer.address_suffix,peer.conn,
        static_cast<unsigned long>(peer.generation),
        static_cast<unsigned long>(peer.count),
        static_cast<unsigned long>(peer.first_ms),
        static_cast<unsigned long>(peer.last_ms),
        static_cast<long>(peer.first_reason),
        static_cast<long>(peer.last_reason));
  for(const auto& fault:r.faults)if(fault.code) {
    Serial.printf("#BLELOG,FAULT,boot=%lu,code=%u,name=%s,count=%lu,first_ms=%lu,last_ms=%lu,first_detail=%lu,last_detail=%lu\n",
        static_cast<unsigned long>(r.boot),fault.code,
        diagnosticErrorName(fault.code),
        static_cast<unsigned long>(fault.count),
        static_cast<unsigned long>(fault.first_ms),
        static_cast<unsigned long>(fault.last_ms),
        static_cast<unsigned long>(fault.first_detail),
        static_cast<unsigned long>(fault.last_detail));
  }
  for(unsigned i=0;i<r.count;++i) {
    const auto& e=r.events[i];
    Serial.printf("#BLELOG,EVENT,boot=%lu,index=%u,seq=%lu,ms=%lu,kind=%u,id=%u,gen=%lu,peer_suffix=%04X,reason_known=%u,reason_raw=%ld,mtu=%u,interval=%u,latency=%u,supervision=%u,rx_bytes=%llu,tx_bytes=%llu,retries=%lu,failed=%lu,code=%u,error=%s,detail=%lu\n",
        static_cast<unsigned long>(r.boot),i,
        static_cast<unsigned long>(e.seq),static_cast<unsigned long>(e.ms),
        e.kind,e.conn,static_cast<unsigned long>(e.gen),
        e.peer_suffix,e.reason_known,static_cast<long>(e.reason),
        e.mtu,e.interval,e.latency,e.supervision,
        static_cast<unsigned long long>(e.rx_bytes),
        static_cast<unsigned long long>(e.tx_bytes),
        static_cast<unsigned long>(e.retries),
        static_cast<unsigned long>(e.failed),
        e.code,diagnosticErrorName(e.code),
        static_cast<unsigned long>(e.detail));
  }
}
void BleEvidenceJournal::index(CanMonitor& can,BleElmTransport& ble) {
  if(!canExport(can,ble))return;
  Serial.println("#BLELOG,INDEX,BEGIN");
  for(unsigned slot=0;slot<kBootSlots;++slot) {
    if(load(slot,scratch_))
      Serial.printf("#BLELOG,INDEX,boot=%lu,total=%lu,kept=%u,queue_drops=%lu\n",
          static_cast<unsigned long>(scratch_.boot),
          static_cast<unsigned long>(scratch_.total),scratch_.count,
          static_cast<unsigned long>(scratch_.queue_drops));
  }
  if(started_ && record_.total && (!persisted_ || dirty_))
    Serial.printf("#BLELOG,INDEX,boot=%lu,state=RAM_ONLY,total=%lu\n",
        static_cast<unsigned long>(record_.boot),
        static_cast<unsigned long>(record_.total));
  Serial.println("#BLELOG,INDEX,END");
}
void BleEvidenceJournal::dump(CanMonitor& can,BleElmTransport& ble,
                              uint32_t boot) {
  if(!canExport(can,ble))return;
  if(boot && boot==record_.boot && record_.total &&
      (!persisted_ || dirty_)) {
    print(record_,false);
    Serial.println("#BLELOG,END");
    return;
  }
  uint32_t wanted=boot;
  if(!wanted) {
    for(unsigned i=0;i<kBootSlots;++i)
      if(load(i,scratch_) && scratch_.boot>wanted)wanted=scratch_.boot;
    if(started_ && record_.total && record_.boot>wanted) {
      print(record_,false);
      Serial.println("#BLELOG,END");
      return;
    }
  }
  if(wanted && load(wanted%kBootSlots,scratch_) &&
     scratch_.boot==wanted) {
    print(scratch_,true);
  } else Serial.printf("#BLELOG,MISSING,boot=%lu\n",
                      static_cast<unsigned long>(wanted));
  Serial.println("#BLELOG,END");
}
} // namespace m5can
#endif
