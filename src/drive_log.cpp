#include "drive_log.h"
#include <Preferences.h>
#include <cstdio>
#include <cstring>

namespace m5can {
DriveLog& driveLog(){static DriveLog log;return log;}
bool DriveLog::load(unsigned slot,Record& record) {
  Preferences nvs;char key[12]{};std::snprintf(key,sizeof(key),"trip%u",slot);
  if(!nvs.begin("m5can_trip",true)) return false;
  const bool loaded=nvs.getBytesLength(key)==sizeof(record) &&
      nvs.getBytes(key,&record,sizeof(record))==sizeof(record);
  nvs.end();
  return loaded && record.magic==kMagic && record.version==kVersion &&
      record.size==sizeof(record) && record.sequence && record.event_count<=8;
}
void DriveLog::begin(uint32_t reset_reason,uint32_t rom_reset_reason) {
  uint32_t latest=0;Record previous{};
  for(unsigned slot=0;slot<kSlots;++slot)
    if(load(slot,previous) && previous.sequence>latest) latest=previous.sequence;
  coordinator_=CoordinatorRecord{};coordinator_enabled_=false;
  record_=Record{};record_.sequence=latest+1;record_.reset_reason=reset_reason;record_.rom_reset_reason=rom_reset_reason;
  std::snprintf(record_.build,sizeof(record_.build),"%s %s",__DATE__,__TIME__);
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
  record_.multipid=1;
#endif
  enabled_=true;save(millis(),true);
  Serial.printf("#DRIVELOG,boot=%lu,slots=%lu,multipid=%u,save_period_ms=30000\n",
      static_cast<unsigned long>(record_.sequence),static_cast<unsigned long>(kSlots),record_.multipid);
}
void DriveLog::coordinator(uint32_t hits,uint32_t misses,uint32_t joins,
                           uint32_t groups,uint32_t cross,uint32_t singles) {
  portENTER_CRITICAL(&mux_);
  coordinator_enabled_=true;coordinator_.sampled_ms=millis();
  coordinator_.hits=hits;coordinator_.misses=misses;coordinator_.joins=joins;
  coordinator_.groups=groups;coordinator_.cross=cross;coordinator_.singles=singles;
  portEXIT_CRITICAL(&mux_);
}
DriveLog::Record DriveLog::snapshot() const {
  portENTER_CRITICAL(&mux_);const Record copy=record_;portEXIT_CRITICAL(&mux_);return copy;
}
void DriveLog::submitted(const DiagnosticRequest& request) {
  if(!enabled_) return;
  portENTER_CRITICAL(&mux_);
  if(request.length==3 && request.data[0]==1 && request.data[1]==0x0C && request.data[2]==0x0D) ++record_.grouped;
  else ++record_.individual;
  portEXIT_CRITICAL(&mux_);
}
void DriveLog::addEvent(Kind kind,uint16_t peer,const DiagnosticResult* result,uint32_t responder) {
  if(!enabled_) return;
  portENTER_CRITICAL(&mux_);
  if(record_.event_count==8) {
    for(unsigned i=1;i<8;++i) record_.events[i-1]=record_.events[i];
    --record_.event_count;
  }
  Event& e=record_.events[record_.event_count++];e=Event{};
  e.kind=static_cast<uint8_t>(kind);e.peer=peer;e.uptime_ms=millis();e.responder=responder;
  if(result) {
    e.status=static_cast<uint8_t>(result->status);e.responses=result->response_count;
    e.transaction_us=result->transaction_us;e.first_response_us=result->tx_to_first_response_us;
    e.raw_count=result->raw_frame_count;
    for(unsigned i=0;i<2 && i<result->raw_frame_count;++i)e.raw[i]=result->raw_frames[i];
  }
  if(kind==Kind::Disconnect) ++record_.disconnects;
  ++record_.events_added;urgent_=true;portEXIT_CRITICAL(&mux_);
}
void DriveLog::outcome(Kind kind,uint16_t peer,const DiagnosticResult& result,uint32_t responder) {
  if(!enabled_) return;
  bool first=false;
  portENTER_CRITICAL(&mux_);
  record_.group_time_us+=result.transaction_us;
  if(result.transaction_us>record_.group_max_us)record_.group_max_us=result.transaction_us;
  if(kind==Kind::GroupEnabled) first=record_.group_ok++==0;
  if(kind==Kind::GroupFallback) ++record_.fallback;
  if(kind==Kind::GroupStopped) ++record_.stopped;
  portEXIT_CRITICAL(&mux_);
  if(kind!=Kind::GroupEnabled || first) addEvent(kind,peer,&result,responder);
}
void DriveLog::event(Kind kind,uint16_t peer){addEvent(kind,peer,nullptr,0);}
bool DriveLog::save(uint32_t now,bool forced,CanMonitor* can) {
  if(!enabled_) return false;
  bool urgent;
  portENTER_CRITICAL(&mux_);urgent=urgent_;portEXIT_CRITICAL(&mux_);
  const uint32_t elapsed=now-last_saved_ms_;
  if(!forced && elapsed<30000 && !(urgent && elapsed>=2000))return false;
  if(can && !can->tryBeginLogCheckpoint())return false;
  Record copy=snapshot();copy.uptime_ms=now;++copy.saves;
  CoordinatorRecord coord{};bool coord_enabled;
  portENTER_CRITICAL(&mux_);coord=coordinator_;coord_enabled=coordinator_enabled_;portEXIT_CRITICAL(&mux_);
  coord.sequence=copy.sequence;coord.uptime_ms=now;
  Preferences nvs;char key[12]{};
  std::snprintf(key,sizeof(key),"trip%u",static_cast<unsigned>(copy.sequence%kSlots));
  bool ok=false;
  if(nvs.begin("m5can_trip",false)) {
    ok=nvs.putBytes(key,&copy,sizeof(copy))==sizeof(copy);
    if(ok && coord_enabled) {
      std::snprintf(key,sizeof(key),"coord%u",static_cast<unsigned>(copy.sequence%kSlots));
      ok=nvs.putBytes(key,&coord,sizeof(coord))==sizeof(coord);
    }
    nvs.end();
  }
  if(can)can->endLogCheckpoint();
  // Rate-limit failed storage too. RAM evidence stays available on failure.
  last_saved_ms_=now;
  portENTER_CRITICAL(&mux_);if(ok)record_.saves=copy.saves;
  if(record_.events_added==copy.events_added)urgent_=false;
  portEXIT_CRITICAL(&mux_);
  if(!ok) Serial.println("#DRIVELOG,storage_error");
  return ok;
}
void DriveLog::resources(uint32_t can_stack,uint32_t ble_stack,uint32_t heap_min,uint32_t largest) {
  portENTER_CRITICAL(&mux_);
  record_.can_stack_free=can_stack;record_.ble_stack_free=ble_stack;
  record_.heap_min=heap_min;record_.largest_heap=largest;
  portEXIT_CRITICAL(&mux_);
}
void DriveLog::poll(CanMonitor& can,BleElmTransport& ble,bool demo) {
  if(!enabled_)return;
  const auto c=can.stats();const auto b=ble.stats();
  const auto lease=can.leaseRemainingMs();
  const auto fault=can.faultLocked();
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  const auto peers=ble.peerTraffic();
  const auto clients=ble.connectedClientCount();
#endif
  portENTER_CRITICAL(&mux_);
  const bool new_fault=fault && !record_.fault;
  coordinator_.owner_cache_hits=c.query_cache_hits;
  record_.uptime_ms=millis();record_.can_rx=c.rx_frames;
  record_.tx_attempts=c.tx_attempts;record_.tx_success=c.tx_success;
  record_.queries=c.query_count;record_.timeouts=c.query_timeout;
  record_.query_ok=c.query_ok;record_.no_data=c.query_no_data;record_.query_failed=c.query_failed;
  record_.drops=c.app_queue_drops;record_.bus_errors=c.driver_bus_error;
  record_.missed=c.driver_rx_missed;record_.overrun=c.driver_rx_overrun;
  record_.lease_ms=lease;record_.fault=fault;record_.demo=demo;
  record_.rx_overflow=b.rx_overflow;record_.notify_failed=b.notify_failures;record_.notify_retries=b.notify_retries;
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  record_.clients=clients;
  for(unsigned i=0;i<2;++i)record_.peers[i]=peers.peers[i].connected?peers.peers[i].address_suffix:0;
#else
  record_.clients=ble.connected()?1:0;
#endif
  portEXIT_CRITICAL(&mux_);
  if(new_fault)event(Kind::CanFault,0);
  save(millis(),false,&can);
}
void DriveLog::dump(CanMonitor& can,BleElmTransport& ble,bool demo) {
  poll(can,ble,demo);save(millis(),true,&can);
  Serial.println("#DRIVELOG,BEGIN,version=2,kind=1:enabled/2:fallback/3:stopped/4:disconnect/5:lease_denied/6:can_fault");
  // Ascending sequence, with the current RAM snapshot last; dump never clears.
  const Record current=snapshot();const uint32_t first=current.sequence>kSlots?current.sequence-kSlots+1:1;
  for(uint32_t seq=first;seq<=current.sequence;++seq) {
    Record r{};
    if(seq==current.sequence)r=current;
    else if(!load(seq%kSlots,r) || r.sequence!=seq)continue;
    Serial.printf("#DRIVELOG,boot=%lu,reset=%lu,rom_reset=%lu,build=%s,multipid=%u,uptime_ms=%lu,saves=%lu,grouped_submissions=%lu,individual_submissions=%lu,group_ok=%lu,fallback=%lu,stopped=%lu,disconnects=%lu,demo=%u,clients=%u,peers=%04X/%04X,fault=%u,lease_ms=%lu\n",
      static_cast<unsigned long>(r.sequence),static_cast<unsigned long>(r.reset_reason),static_cast<unsigned long>(r.rom_reset_reason),r.build,r.multipid,
      static_cast<unsigned long>(r.uptime_ms),static_cast<unsigned long>(r.saves),
      static_cast<unsigned long>(r.grouped),static_cast<unsigned long>(r.individual),static_cast<unsigned long>(r.group_ok),
      static_cast<unsigned long>(r.fallback),static_cast<unsigned long>(r.stopped),static_cast<unsigned long>(r.disconnects),
      r.demo,r.clients,r.peers[0],r.peers[1],r.fault,static_cast<unsigned long>(r.lease_ms));
    Serial.printf("#DRIVELOG,CAN,boot=%lu,rx=%llu,tx=%llu/%llu,q=%llu,ok=%llu,no_data=%llu,failed=%llu,timeout=%llu,drop=%llu,buserr=%lu,missed=%lu,overrun=%lu,BLE_rx_overflow=%lu,notify_failed=%lu,retries=%lu\n",
      static_cast<unsigned long>(r.sequence),static_cast<unsigned long long>(r.can_rx),static_cast<unsigned long long>(r.tx_success),static_cast<unsigned long long>(r.tx_attempts),
      static_cast<unsigned long long>(r.queries),static_cast<unsigned long long>(r.query_ok),static_cast<unsigned long long>(r.no_data),static_cast<unsigned long long>(r.query_failed),
      static_cast<unsigned long long>(r.timeouts),static_cast<unsigned long long>(r.drops),
      static_cast<unsigned long>(r.bus_errors),static_cast<unsigned long>(r.missed),static_cast<unsigned long>(r.overrun),
      static_cast<unsigned long>(r.rx_overflow),static_cast<unsigned long>(r.notify_failed),static_cast<unsigned long>(r.notify_retries));
    Serial.printf("#DRIVELOG,TIMING,boot=%lu,group_outcomes=%lu,group_total_us=%llu,group_max_us=%lu\n",
      static_cast<unsigned long>(r.sequence),static_cast<unsigned long>(r.group_ok+r.fallback+r.stopped),
      static_cast<unsigned long long>(r.group_time_us),static_cast<unsigned long>(r.group_max_us));
    Serial.printf("#DRIVELOG,MEMORY,boot=%lu,can_stack_free_bytes=%lu,ble_stack_free_bytes=%lu,heap_min=%lu,largest_heap=%lu\n",
      static_cast<unsigned long>(r.sequence),static_cast<unsigned long>(r.can_stack_free),
      static_cast<unsigned long>(r.ble_stack_free),static_cast<unsigned long>(r.heap_min),static_cast<unsigned long>(r.largest_heap));
    CoordinatorRecord coord{};bool loaded=false;
    if(seq==current.sequence) {
      portENTER_CRITICAL(&mux_);coord=coordinator_;loaded=coordinator_enabled_;portEXIT_CRITICAL(&mux_);
      coord.sequence=seq;coord.uptime_ms=current.uptime_ms;
    } else {
      Preferences nvs;char key[12]{};
      std::snprintf(key,sizeof(key),"coord%u",static_cast<unsigned>(seq%kSlots));
      if(nvs.begin("m5can_trip",true)) {
        loaded=nvs.getBytesLength(key)==sizeof(coord) &&
            nvs.getBytes(key,&coord,sizeof(coord))==sizeof(coord);
        nvs.end();
      }
    }
    if(loaded && coord.magic==0x4D35434FU && coord.version==1 &&
        coord.sequence==seq && coord.uptime_ms==r.uptime_ms) {
      Serial.printf("#DRIVELOG,COORD,boot=%lu,sampled_ms=%lu,cache_hits=%lu,cache_lookup_misses=%lu,joins=%lu,grouped_submissions=%lu,cross_group_submissions=%lu,single_submissions=%lu,owner_cache_hits=%llu\n",
        static_cast<unsigned long>(seq),static_cast<unsigned long>(coord.sampled_ms),
        static_cast<unsigned long>(coord.hits),static_cast<unsigned long>(coord.misses),
        static_cast<unsigned long>(coord.joins),static_cast<unsigned long>(coord.groups),
        static_cast<unsigned long>(coord.cross),static_cast<unsigned long>(coord.singles),
        static_cast<unsigned long long>(coord.owner_cache_hits));
    }
    for(unsigned i=0;i<r.event_count;++i) {
      const auto& e=r.events[i];Serial.printf("#DRIVELOG,EVENT,boot=%lu,ms=%lu,kind=%u,peer=%u,status=%u,ecu=%08lX,responses=%u,raw_count=%u,transaction_us=%lu,first_response_us=%lu\n",
        static_cast<unsigned long>(r.sequence),static_cast<unsigned long>(e.uptime_ms),e.kind,e.peer,e.status,static_cast<unsigned long>(e.responder),e.responses,e.raw_count,
        static_cast<unsigned long>(e.transaction_us),static_cast<unsigned long>(e.first_response_us));
      for(unsigned j=0;j<2 && j<e.raw_count;++j) {
        const auto& f=e.raw[j];Serial.printf("#DRIVELOG,RAW,boot=%lu,event=%u,id=%08lX,dlc=%u,data=",
          static_cast<unsigned long>(r.sequence),i,static_cast<unsigned long>(f.can_id),f.dlc);
        for(unsigned k=0;k<f.dlc && k<8;++k)Serial.printf("%02X",f.data[k]);
        Serial.println("");
      }
    }
  }
  Serial.println("#DRIVELOG,END");
}
} // namespace m5can
