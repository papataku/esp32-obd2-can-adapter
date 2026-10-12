#include "ble_evidence_journal.h"
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
#include <Preferences.h>
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
  Preferences prefs;
  char key[12]{};
  std::snprintf(key,sizeof(key),"ble%u",slot);
  if(!prefs.begin("m5can_blelog",true))return false;
  const bool present=prefs.getBytesLength(key)==sizeof(target) &&
      prefs.getBytes(key,&target,sizeof(target))==sizeof(target);
  prefs.end();
  if(!present || target.magic!=kMagic || target.version!=kVersion ||
     target.bytes!=sizeof(Record) || !target.boot ||
     target.count>kEvents)return false;
  const uint32_t saved_crc=target.crc;
  target.crc=0;
  const bool good=checksum(target)==saved_crc;
  target.crc=saved_crc;
  return good;
}
void BleEvidenceJournal::begin(uint32_t boot,const char* build) {
  record_=Record{};
  scratch_=Record{};
  record_.boot=boot;
  if(build)std::snprintf(record_.build,sizeof(record_.build),"%s",build);
  started_=boot!=0;
  dirty_=persisted_=urgent_=false;
  last_attempt_ms_=last_success_ms_=0;
}
void BleEvidenceJournal::observeQueueDrops(uint32_t drops) {
  if(!started_ || drops==record_.queue_drops)return;
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
  out.rx_bytes=event.rx_bytes;out.tx_bytes=event.tx_bytes;
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
  Preferences prefs;
  char key[12]{};
  std::snprintf(key,sizeof(key),"ble%u",record_.boot%kBootSlots);
  bool wrote=false;
  if(prefs.begin("m5can_blelog",false)) {
    wrote=prefs.putBytes(key,&scratch_,sizeof(scratch_))==sizeof(scratch_);
    prefs.end();
  }
  Record verified{};
  const bool ok=wrote && load(record_.boot%kBootSlots,verified) &&
      verified.boot==record_.boot && verified.saves==scratch_.saves;
  can.endLogCheckpoint();
  if(ok) {
    record_.saves=scratch_.saves;
    dirty_=urgent_=false;
    persisted_=true;
    last_success_ms_=now;
  } else {
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
  for(unsigned i=0;i<r.count;++i) {
    const auto& e=r.events[i];
    Serial.printf("#BLELOG,EVENT,boot=%lu,index=%u,seq=%lu,ms=%lu,kind=%u,id=%u,gen=%lu,peer_suffix=%04X,reason_known=%u,reason_raw=%ld,mtu=%u,interval=%u,latency=%u,supervision=%u,rx_bytes=%llu,tx_bytes=%llu,retries=%lu,failed=%lu\n",
        static_cast<unsigned long>(r.boot),i,
        static_cast<unsigned long>(e.seq),static_cast<unsigned long>(e.ms),
        e.kind,e.conn,static_cast<unsigned long>(e.gen),
        e.peer_suffix,e.reason_known,static_cast<long>(e.reason),
        e.mtu,e.interval,e.latency,e.supervision,
        static_cast<unsigned long long>(e.rx_bytes),
        static_cast<unsigned long long>(e.tx_bytes),
        static_cast<unsigned long>(e.retries),
        static_cast<unsigned long>(e.failed));
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
