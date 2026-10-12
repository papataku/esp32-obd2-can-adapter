// Real dual-session scheduler/ELM/notification path with a host ECU backend.
// This is not a physical CAN bench or a test of TWAI electrical behavior.
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include "drive_log.h"
#include "dual_ble_elm.h"
#include "serial_protocol.h"
using namespace m5can;
namespace {
enum class Ecu { Full, Partial, Negative, Silent, Split, FirstFrame, Foreign,
                 Lease, BusOff, Driver };
Ecu mode=Ecu::Full;
bool demo=false, hold=false, ready=false;
DiagnosticRequest queued{};
std::vector<DiagnosticRequest> requests;
DiagnosticResult completed{};
void feed(DiagnosticTransaction& tx, uint32_t ecu,
          std::initializer_list<uint8_t> payload, bool ff=false) {
  TransactionFrame frame{};
  frame.can_id=ecu; frame.extended=true; frame.dlc=8;
  frame.timestamp_us=static_cast<uint64_t>(millis())*1000+1;
  if (ff) {
    const uint8_t bad[]={0x10,8,0x41,0x0C,0x13,0x88,0x0D,0x50};
    std::memcpy(frame.data,bad,8);
  } else {
    frame.data[0]=static_cast<uint8_t>(payload.size());
    size_t i=1; for(uint8_t byte:payload) frame.data[i++]=byte;
  }
  const auto step=tx.onFrame(frame);
  assert(step.event!=TransactionEvent::NeedFlowControl);
}
DiagnosticResult ecuReply(const DiagnosticRequest& r) {
  DiagnosticTransaction tx;
  tx.begin(r,static_cast<uint64_t>(millis())*1000);
  if(r.length==3 && r.data[0]==1) {
    switch(mode) {
      case Ecu::Full: feed(tx,0x18DAF101,{0x41,0x0C,0x13,0x88,0x0D,0x50}); break;
      case Ecu::Foreign: feed(tx,0x18DAF102,{0x41,0x0C,0x13,0x88,0x0D,0x50}); break;
      case Ecu::Partial: feed(tx,0x18DAF101,{0x41,0x0C,0x13,0x88}); break;
      case Ecu::Negative: feed(tx,0x18DAF101,{0x7F,1,0x12}); break;
      case Ecu::Split:
        feed(tx,0x18DAF101,{0x41,0x0C,0x13,0x88});
        feed(tx,0x18DAF102,{0x41,0x0D,0x50}); break;
      case Ecu::FirstFrame: feed(tx,0x18DAF101,{},true); break;
      default: break;
    }
  } else if(r.data[1]==0x0C) feed(tx,0x18DAF101,{0x41,0x0C,0x13,0x88});
  else if(r.data[1]==0x0D) feed(tx,0x18DAF101,{0x41,0x0D,0x50});
  else feed(tx,0x18DAF101,{0x41,0x05,0x67});
  DiagnosticResult result=tx.result();
  result.status=result.raw_frame_count?DiagnosticStatus::Ok:DiagnosticStatus::NoData;
  if(r.length==3) {
    if(mode==Ecu::Lease) result.status=DiagnosticStatus::LeaseRequired;
    if(mode==Ecu::BusOff) result.status=DiagnosticStatus::BusOff;
    if(mode==Ecu::Driver) result.status=DiagnosticStatus::ModeError;
  }
  return result;
}
std::string output(uint16_t id) {
  std::string result;
  for(const auto& n:test_notifications) if(n.conn==id)
    result.append(n.data.begin(),n.data.end());
  return result;
}
void write(ble_gap_conn_desc& peer,const char* command) {
  NimBLECharacteristic characteristic;
  NimBLECharacteristic::next_write_value=std::string(command)+"\r";
  NimBLECharacteristic::callbacks->onWrite(&characteristic,&peer);
}
struct Bench {
  CanMonitor can;
  BleElmTransport ble;
  SerialProtocol serial{can,&ble};
  DualBleElm dual{can,ble,serial};
  ble_gap_conn_desc a{7}, b{19};
  Bench() {
    ready=false;hold=false;demo=false;requests.clear();test_notifications.clear();
    g_test_millis=1000;
    assert(ble.begin());
    driveLog().begin(1);
    NimBLEServer::callbacks->onConnect(NimBLEDevice::createServer(),&a);
    NimBLEServer::callbacks->onConnect(NimBLEDevice::createServer(),&b);
    dual.poll();
    write(a,"ATE0"); write(b,"ATE0"); dual.poll();
    write(a,"ATH1"); write(b,"ATH1"); dual.poll();
    write(a,"ATL0"); write(b,"ATL0"); dual.poll();
    write(a,"ATS1"); write(b,"ATS1"); dual.poll();
    test_notifications.clear();
  }
  void batchBoth() {
    write(a,"ATM5B00:010C,010D,0105");
    write(b,"ATM5B00:010C,010D,0105");
  }
  void drain() {for(int i=0;i<40;++i){dual.poll();g_test_millis+=50;}}
  size_t groups() const {
    size_t n=0;for(const auto& r:requests) if(r.length==3 && r.data[0]==1)++n;
    return n;
  }
  void values() const {
    for(uint16_t id:{7,19}) {
      const auto text=output(id);
      if(text.find("M5ITEM:00:010C\r18DAF101 04 41 0C 13 88")==std::string::npos) std::cerr << "peer=" << id << " mode=" << static_cast<int>(mode) << " output=" << text << "\n";
      assert(text.find("M5ITEM:00:010C\r18DAF101 04 41 0C 13 88")!=std::string::npos);
      assert(text.find("M5ITEM:00:010D\r18DAF101 03 41 0D 50")!=std::string::npos);
      assert(text.find("M5DONE\r>")!=std::string::npos);
      assert(text.find("NO DATA")==std::string::npos);
    }
  }
};
}
namespace m5can {
bool SerialProtocol::demoActive() const {return demo;}
bool CanMonitor::acquireLease(uint32_t duration) {
  if(fault_locked_) return false;
  lease_deadline_ms_=millis()+duration; return true;
}
void CanMonitor::revokeLease(){lease_deadline_ms_=0;}
uint32_t CanMonitor::leaseRemainingMs() const {
  return lease_deadline_ms_>millis()?lease_deadline_ms_-millis():0;
}
CanStats CanMonitor::stats() const {return stats_;}
bool CanMonitor::submitQuery(const DiagnosticRequest& r) {
  if(log_checkpoint_active_)return false;
  assert(!query_in_flight_); assert(leaseRemainingMs());
  assert(DiagnosticPolicy::allowedReadRequest(r));
  requests.push_back(r);queued=r;query_in_flight_=true;ready=true;return true;
}
bool CanMonitor::pollQueryResult(DiagnosticResult& result) {
  if(!ready || hold) return false;
  completed=ecuReply(queued);result=completed;ready=false;query_in_flight_=false;
  if(result.status==DiagnosticStatus::BusOff || result.status==DiagnosticStatus::ModeError)
    fault_locked_=true;
  return true;
}
}
int main() {
#ifndef M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL
  mode=Ecu::Full; Bench stable; stable.batchBoth(); stable.drain(); stable.values();
  assert(stable.groups()==0 && requests.size()==6);
  std::cout << "PASS stable dual profiles preserve individual CAN requests\n";
  return 0;
#endif
  {mode=Ecu::Full;Bench bench;bench.batchBoth();bench.drain();bench.values();
   assert(bench.groups()==2 && requests.size()==4);
   assert(driveLog().snapshot().grouped==2 && driveLog().snapshot().group_ok==2);
   assert(driveLog().snapshot().individual==2);
   bench.batchBoth();bench.drain();assert(bench.groups()==4 && requests.size()==8);}
  for(Ecu failure:{Ecu::Partial,Ecu::Negative,Ecu::Silent,Ecu::Split,Ecu::FirstFrame}) {
    mode=failure;Bench bench;bench.batchBoth();bench.drain();bench.values();
    assert(bench.groups()==1 && requests.size()==7);
    assert(driveLog().snapshot().fallback==1 && driveLog().snapshot().grouped==1);
    bench.batchBoth();bench.drain();bench.values();
    assert(bench.groups()==1 && requests.size()==13);
  }
  {mode=Ecu::Full;Bench bench;write(bench.a,"ATM5B00:010C,010D,0105");bench.drain();
   mode=Ecu::Foreign;bench.batchBoth();bench.drain();bench.values();
   assert(bench.groups()==2);bench.batchBoth();bench.drain();assert(bench.groups()==2);}
  for(Ecu failure:{Ecu::Lease,Ecu::BusOff,Ecu::Driver}) {
    mode=failure;Bench bench;write(bench.a,"ATM5B00:010C,010D,0105");bench.drain();
    assert(requests.size()==1 && bench.groups()==1);
   assert(driveLog().snapshot().stopped==1);
    assert(output(7).find("M5DONE\r>")!=std::string::npos);
  }
  {mode=Ecu::Full;Bench bench;hold=true;bench.batchBoth();bench.dual.poll();
   assert(bench.groups()==1);
   NimBLEServer::callbacks->onDisconnect(NimBLEDevice::createServer(),&bench.a);
   bench.dual.poll();hold=false;bench.drain();
   assert(bench.groups()==1 && requests.size()==4);
   assert(output(7).empty());assert(output(19).find("M5DONE\r>")!=std::string::npos);}
  {mode=Ecu::Full;Bench bench;demo=true;bench.batchBoth();bench.drain();
   assert(requests.empty());for(uint16_t id:{7,19}) assert(output(id).find("M5DONE\r>")!=std::string::npos);}
  // NVS survives recorder reconstruction; writes are periodic/event-only,
  // ring slots remain bounded and corrupt/truncated entries are rejected.
  {
    Bench bench;Preferences::storage.clear();Preferences::writes=0;
    DriveLog journal;g_test_millis=1000;journal.begin(1,21);
    DiagnosticRequest r{};r.length=3;r.data[0]=1;r.data[1]=0x0C;r.data[2]=0x0D;
    DiagnosticResult result{};result.status=DiagnosticStatus::Ok;
    result.raw_frame_count=1;result.response_count=1;
    result.transaction_us=12000;result.tx_to_first_response_us=8000;
    result.raw_frames[0].can_id=0x18DAF101;result.raw_frames[0].dlc=8;
    const uint8_t bytes[]={6,0x41,0x0C,0x13,0x88,0x0D,0x50,0};
    std::memcpy(result.raw_frames[0].data,bytes,8);
    for(unsigned i=0;i<100;++i) {
      journal.submitted(r);journal.outcome(DriveLog::Kind::GroupEnabled,7,result,0x18DAF101);
      ++g_test_millis;journal.poll(bench.can,bench.ble,false);
    }
    assert(Preferences::writes==1);
    assert(journal.snapshot().group_ok==100 && journal.snapshot().event_count==1);
    g_test_millis=3000;
    hold=true;write(bench.a,"ATM5B00:010C,010D,0105");bench.dual.poll();
    assert(bench.can.queryPending());
    assert(!bench.can.tryBeginLogCheckpoint());
    journal.poll(bench.can,bench.ble,false);assert(Preferences::writes==1);
    hold=false;bench.drain();
    assert(bench.can.tryBeginLogCheckpoint());
    assert(!bench.can.submitQuery(r));bench.can.endLogCheckpoint();
    g_test_millis=6000;journal.poll(bench.can,bench.ble,false);
    assert(Preferences::writes==2);
    DriveLog restarted;restarted.begin(9);assert(restarted.snapshot().sequence==2);
    Preferences nvs;assert(nvs.begin("m5can_trip",true));DriveLog::Record saved{};
    assert(nvs.getBytes("trip1",&saved,sizeof(saved))==sizeof(saved));
    assert(saved.grouped==100 && saved.group_ok==100 && saved.event_count==1);
    assert(saved.events[0].responder==0x18DAF101);
    assert(std::memcmp(saved.events[0].raw[0].data,bytes,8)==0);
    assert(saved.uptime_ms==6000 && saved.rom_reset_reason==21);
    assert(saved.group_time_us==1200000 && saved.group_max_us==12000);
    assert(saved.events[0].transaction_us==12000 && saved.events[0].first_response_us==8000);
    for(unsigned i=0;i<12;++i){DriveLog reboot;reboot.begin(1);}
    assert(Preferences::storage.size()==DriveLog::kSlots);
    Preferences::storage["m5can_trip/trip0"]={1,2,3};
    DriveLog recover;recover.begin(1);assert(recover.snapshot().sequence==15);
    saved.version=1;saved.sequence=9999;
    Preferences::storage["m5can_trip/trip0"].resize(sizeof(saved));
    std::memcpy(Preferences::storage["m5can_trip/trip0"].data(),&saved,sizeof(saved));
    DriveLog version_recover;version_recover.begin(1);assert(version_recover.snapshot().sequence==16);
    journal.dump(bench.can,bench.ble,false); // Never deletes older entries.
    assert(Preferences::storage.size()==DriveLog::kSlots);
  }
  std::cout << "PASS actual dual BLE scheduler with host ECU: grouping, one probe, fallback, routing, faults, disconnect, DEMO\n";
}
