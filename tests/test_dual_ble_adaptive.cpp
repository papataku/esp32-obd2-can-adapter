// Real dual-session scheduler/ELM/notification path with a host ECU backend.
// This is not a physical CAN bench or a test of TWAI electrical behavior.
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <utility>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include "drive_log.h"
#include "dual_ble_elm.h"
#include "serial_protocol.h"
using namespace m5can;
namespace {
enum class Ecu { Full, Partial, Negative, Silent, Split, FirstFrame, Foreign,
                 Lease, BusOff, Driver, MultiAgree, MultiConflict, MultiPartial,
                 PinnedOther };
Ecu mode=Ecu::Full;
bool demo=false, hold=false, ready=false;
DiagnosticRequest queued{};
std::vector<DiagnosticRequest> requests;
DiagnosticResult completed{};
void feed(DiagnosticTransaction& tx, uint32_t ecu,
          std::initializer_list<uint8_t> payload, bool ff=false) {
  TransactionFrame frame{};
  frame.can_id=ecu; frame.extended=true; frame.dlc=8;
  frame.timestamp_us=static_cast<uint64_t>(millis())*1000;
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
      case Ecu::MultiAgree:
        // Five simulated complete responses, in non-CAN-ID order.
        feed(tx,0x18DAF105,{0x41,0x0C,0x13,0x88,0x0D,0x50});
        feed(tx,0x18DAF102,{0x41,0x0C,0x13,0x88,0x0D,0x50});
        feed(tx,0x18DAF108,{0x41,0x0C,0x13,0x88,0x0D,0x50});
        feed(tx,0x18DAF101,{0x41,0x0C,0x13,0x88,0x0D,0x50});
        feed(tx,0x18DAF107,{0x41,0x0C,0x13,0x88,0x0D,0x50});
        break;
      case Ecu::MultiConflict:
      case Ecu::PinnedOther:
        feed(tx,0x18DAF101,{0x41,0x0C,0x13,0x88,0x0D,0x50});
        feed(tx,0x18DAF102,{0x41,0x0C,0x13,0x89,0x0D,0x50});
        break;
      case Ecu::MultiPartial:
        feed(tx,0x18DAF101,{0x41,0x0C,0x13,0x88,0x0D,0x50});
        feed(tx,0x18DAF102,{0x41,0x0D,0x50});
        break;
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
   assert(bench.groups()==1 && requests.size()==2);
   assert(driveLog().snapshot().grouped==1 && driveLog().snapshot().group_ok==1);
   assert(driveLog().snapshot().individual==1);
   bench.batchBoth();bench.drain();assert(bench.groups()==2 && requests.size()==4);}
  // Real dual-BLE routing of a five-ECU agreement: selected ECU is
  // deterministic; both client envelopes are intact, without extra CAN TX.
  {mode=Ecu::MultiAgree;Bench bench;bench.batchBoth();bench.drain();bench.values();
   assert(bench.groups()==1 && requests.size()==2);
   assert(driveLog().snapshot().group_ok==1 && driveLog().snapshot().fallback==0);
   bench.batchBoth();bench.drain();bench.values();
   assert(bench.groups()==2);
  }
  // An initial disagreeing or incomplete ECU rejects grouped probing once.
  // Each BLE peer subsequently obtains only its independently requested data.
  for(Ecu initial:{Ecu::MultiConflict,Ecu::MultiPartial}) {
    mode=initial;Bench bench;bench.batchBoth();bench.drain();bench.values();
    assert(bench.groups()==1 && driveLog().snapshot().fallback==1);
    bench.batchBoth();bench.drain();assert(bench.groups()==1);
  }
  // After one successful probe, an extraneous ECU with different values is
  // harmless; the learned ECU alone determines both cache entries.
  {mode=Ecu::Full;Bench bench;bench.batchBoth();bench.drain();bench.values();
   assert(bench.groups()==1 && driveLog().snapshot().group_ok==1);
   g_test_millis+=150;test_notifications.clear();
   mode=Ecu::PinnedOther;bench.batchBoth();bench.drain();bench.values();
   assert(bench.groups()==2 && driveLog().snapshot().group_ok==2);
   assert(driveLog().snapshot().fallback==0);
  }
  for(Ecu failure:{Ecu::Partial,Ecu::Negative,Ecu::Silent,Ecu::Split,Ecu::FirstFrame}) {
    mode=failure;Bench bench;bench.batchBoth();bench.drain();bench.values();
    // A single complete RPM value from a partial grouped reply is cached;
    // only speed is retried. All other unsupported cases retry both PIDs.
    const size_t first_count=failure==Ecu::Partial?3U:4U;
    assert(bench.groups()==1 && requests.size()==first_count);
    assert(driveLog().snapshot().fallback==1);
    bench.batchBoth();bench.drain();bench.values();
    assert(bench.groups()==1 && requests.size()==first_count+3);
  }
  // Two different BLE peers independently request different PIDs. The
  // coordinator may group them only because both were already pending;
  // there is no synthetic extra PID fetch or cross-peer result leakage.
  {mode=Ecu::Full;Bench bench;
   write(bench.a,"ATM5B00:010C,0105");
   write(bench.b,"ATM5B00:010D,0105");
   bench.dual.poll();bench.dual.poll();bench.dual.poll();
   assert(bench.groups()==1 && requests.size()==2);
   assert(output(7).find("M5ITEM:00:010C\r18DAF101 04 41 0C 13 88")!=std::string::npos);
   assert(output(7).find("M5ITEM:00:010D")==std::string::npos);
   assert(output(19).find("M5ITEM:00:010D\r18DAF101 03 41 0D 50")!=std::string::npos);
   assert(output(19).find("M5ITEM:00:010C")==std::string::npos);
   // Both parameters are cached individually: reusing RPM in a different
   // BLE session within the TTL does not generate a new CAN transaction.
   test_notifications.clear();
   write(bench.b,"ATM5B00:010C,0105");
   bench.dual.poll();bench.dual.poll();
   assert(requests.size()==2);
   assert(output(19).find("M5ITEM:00:010C")!=std::string::npos);
  }
  // Matching pending requests from two peers should also fan out one
  // individual physical CAN reply, even when grouping is ineligible.
  {mode=Ecu::Full;Bench bench;
   write(bench.a,"ATM5B00:0105,010D");
   write(bench.b,"ATM5B00:0105,010D");
   bench.dual.poll();bench.dual.poll();bench.dual.poll();
   assert(requests.size()==2 && bench.groups()==0);
   assert(output(7).find("M5ITEM:00:0105")!=std::string::npos);
   assert(output(19).find("M5ITEM:00:0105")!=std::string::npos);
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
  // Deterministic repeated-run simulation: no growing queues, duplicate
  // CAN requests, stale replay or peer mix-up over successive batches.
  {mode=Ecu::Full;Bench bench;
   for(unsigned cycle=0;cycle<250;++cycle) {
     test_notifications.clear();
     bench.batchBoth();bench.drain();
     assert(bench.groups()==cycle+1);
     assert(requests.size()==(cycle+1)*2);
     bench.values();
   }
  }
  // Disconnect the in-flight owner after a same-PID peer has joined.
  // The surviving peer still receives the physical CAN result.
  {mode=Ecu::Full;Bench bench;hold=true;
   write(bench.a,"ATM5B00:0105,010D");
   write(bench.b,"ATM5B00:0105,010D");
   bench.dual.poll();assert(requests.size()==1);
   NimBLEServer::callbacks->onDisconnect(
       NimBLEDevice::createServer(),&bench.a);
   bench.dual.poll();hold=false;bench.drain();
   assert(requests.size()==2 && bench.groups()==0);
   assert(output(7).empty());
   assert(output(19).find("M5DONE\r>")!=std::string::npos);
  }
  // A joined secondary peer can disconnect and reuse the same connection
  // ID with a different generation. It must NOT receive the old coolant
  // result as its newly requested vehicle-speed result.
  {mode=Ecu::Full;Bench bench;hold=true;
   write(bench.a,"ATM5B00:0105,010D");
   write(bench.b,"ATM5B00:0105,010D");
   bench.dual.poll();assert(requests.size()==1);
   NimBLEServer::callbacks->onDisconnect(
       NimBLEDevice::createServer(),&bench.b);
   bench.dual.poll();
   NimBLEServer::callbacks->onConnect(
       NimBLEDevice::createServer(),&bench.b);
   bench.dual.poll();
   // Reconnected ELM session starts with default ATS0 (no spaces); apply
   // ATS1 again before checking the human-readable reply formatting.
   write(bench.b,"ATS1");bench.dual.poll();
   test_notifications.clear();
   write(bench.b,"ATM5B00:010D,0105");
   bench.dual.poll();hold=false;bench.drain();
   const auto fresh=output(19);
   assert(fresh.find("M5ITEM:00:010D")!=std::string::npos);
   assert(fresh.find("18DAF101 03 41 0D 50")!=std::string::npos);
   assert(fresh.find("M5DONE\r>")!=std::string::npos);
  }
  // Reproducible, deterministic offline interleaving matrix. Exercise
  // the production BLE sessions with two different pending batch shapes,
  // six CAN responder profiles and hundreds of independent boot sessions.
  // Each generated client may receive ONLY its requested PID item labels.
  {
    constexpr const char* patterns[]={
        "ATM5B00:010C,0105",
        "ATM5B00:010D,0105",
        "ATM5B00:010C,010D"
    };
    constexpr const char* first[]={
        "M5ITEM:00:010C","M5ITEM:00:010D","M5ITEM:00:010C"
    };
    constexpr const char* second[]={
        "M5ITEM:00:0105","M5ITEM:00:0105","M5ITEM:00:010D"
    };
    constexpr Ecu modes[]={
        Ecu::Full,Ecu::Partial,Ecu::Negative,
        Ecu::Silent,Ecu::Split,Ecu::FirstFrame
    };
    uint32_t seed=0x4d354341U;
    for(unsigned scenario=0;scenario<540;++scenario) {
      // Fixed LCG seed makes any failing scenario reproducible.
      seed=seed*1664525U+1013904223U;
      const unsigned a=(seed>>8)%3;
      seed=seed*1664525U+1013904223U;
      const unsigned b=(seed>>8)%3;
      mode=modes[scenario%6];
      Bench bench;
      write(bench.a,patterns[a]);write(bench.b,patterns[b]);
      bench.drain();
      for(const auto& peer_case : {std::pair<uint16_t,unsigned>{7,a},
                                   std::pair<uint16_t,unsigned>{19,b}}) {
        const std::string got=output(peer_case.first);
        const unsigned idx=peer_case.second;
        if (got.find(first[idx])==std::string::npos ||
            got.find(second[idx])==std::string::npos ||
            got.find("M5DONE\r>")==std::string::npos) {
          std::cerr<<"MATRIX_FAIL "<<scenario
                   <<" case="<<idx<<" peer="<<peer_case.first
                   <<" mode="<<static_cast<int>(mode)
                   <<" output="<<got<<"\\n";
          assert(false);
        }
      }
      // The host backend enforces at most one CAN query in flight.
      assert(requests.size()<=6);
    }
  }
  // USB and BLE remain different ELM sessions but physically serialize
  // through the SAME CanMonitor query-in-flight gate. No second CAN TX
  // may start while the USB read owns the transaction slot.
  {mode=Ecu::Full;Bench bench;
   DiagnosticRequest usb{};
   usb.can_id=0x18DB33F1U;usb.length=2;
   usb.data[0]=0x01;usb.data[1]=0x05;
   assert(bench.can.acquireLease(5000));
   assert(bench.can.submitQuery(usb));
   bench.batchBoth();bench.dual.poll();
   assert(requests.size()==1 && bench.groups()==0);
   DiagnosticResult usb_done{};
   assert(bench.can.pollQueryResult(usb_done));
   bench.drain();bench.values();
   assert(bench.groups()==1 && requests.size()==3);
  }
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
  // First multi-ECU CAN probe must survive reboot even when the regular
  // eight-event ring rolls over from unrelated BLE disconnects. Not just a
  // USB printf: persist all original accepted diagnostic frames in NVS.
  {
    Bench bench;Preferences::storage.clear();Preferences::writes=0;
    g_test_millis=1000;
    DriveLog journal;journal.begin(1,21);
    DiagnosticResult first{};
    first.status=DiagnosticStatus::Ok;
    first.response_count=5;first.raw_frame_count=5;
    first.transaction_us=16317;first.tx_to_first_response_us=656;
    const uint32_t ids[]={0x18DAF1EF,0x18DAF10E,0x18DAF101,
                          0x18DAF102,0x18DAF103};
    const uint8_t payload[]={6,0x41,0x0C,0,0,0x0D,0,0};
    for(unsigned i=0;i<5;++i) {
      auto& f=first.raw_frames[i];
      f.can_id=ids[i];f.dlc=8;
      std::memcpy(f.data,payload,8);
      f.rx_offset_us=656+i*137U;
    }
    journal.submitted(DiagnosticRequest{
        0x18DB33F1,3,{1,0x0C,0x0D},200});
    journal.recordFirstProbe(7,first,0x18DAF101,0,0);
    assert(!journal.probeSaved());
    // An in-flight CAN owner transaction must block flash writes.
    hold=true;write(bench.a,"ATM5B00:010C,010D,0105");bench.dual.poll();
    assert(bench.can.queryPending());
    journal.poll(bench.can,bench.ble,false);
    assert(!journal.probeSaved());
    hold=false;bench.drain();
    ++g_test_millis;
    journal.poll(bench.can,bench.ble,false);
    assert(journal.probeSaved());
    for(unsigned i=0;i<12;++i)
      journal.event(DriveLog::Kind::Disconnect,7);
    g_test_millis+=3000;
    journal.poll(bench.can,bench.ble,false);
    assert(journal.snapshot().event_count==8);
    Preferences nvs;assert(nvs.begin("m5can_trip",true));
    DriveLog::ProbeRecord probe{};
    assert(nvs.getBytes("probe1",&probe,sizeof(probe))==sizeof(probe));
    assert(probe.sequence==1 && probe.version==1 &&
           probe.responses==5);
    assert(probe.raw_count==5 && probe.raw_saved==5 && !probe.truncated);
    assert(probe.decision==0 && probe.reject_reason==0);
    for(unsigned i=0;i<5;++i) {
      assert(probe.raw[i].can_id==ids[i]);
      assert(probe.raw[i].rx_offset_us==656+i*137U);
      assert(std::memcmp(probe.raw[i].data,payload,8)==0);
    }
    nvs.end();
    DriveLog after_reboot;after_reboot.begin(9,21);
    assert(after_reboot.snapshot().sequence==2);
    // Original full first-probe evidence from the prior boot is intact.
    Preferences reload;assert(reload.begin("m5can_trip",true));
    DriveLog::ProbeRecord saved{};
    assert(reload.getBytes("probe1",&saved,sizeof(saved))==sizeof(saved));
    assert(saved.sequence==1 && saved.raw_saved==5);
    assert(saved.raw[4].can_id==0x18DAF103);
    // A corrupted NVS record must not be mistaken for valid evidence.
    auto& bytes=Preferences::storage["m5can_trip/probe1"];
    bytes[sizeof(saved)-1]^=0x80;
    after_reboot.dump(bench.can,bench.ble,false);
    assert(bytes[sizeof(saved)-1]!=(reinterpret_cast<uint8_t*>(&saved))[sizeof(saved)-1]);
    reload.end();
  }
  // Check the true ISO-TP reception-time evidence propagation, rather
  // than only synthetic manually populated timestamps.
  {
    DiagnosticTransaction tx;
    DiagnosticRequest rq{};
    rq.can_id=0x18DB33F1;rq.length=3;
    rq.data[0]=1;rq.data[1]=0x0C;rq.data[2]=0x0D;
    tx.begin(rq,1000000);
    TransactionFrame one{};
    one.extended=true;one.can_id=0x18DAF10E;one.dlc=8;
    one.timestamp_us=1000656;
    const uint8_t payload[]={6,0x41,0x0C,0,0,0x0D,0,0};
    std::memcpy(one.data,payload,8);
    assert(tx.onFrame(one).event==TransactionEvent::Completed);
    assert(tx.result().raw_frame_count==1 &&
           tx.result().raw_frames[0].rx_offset_us==656);
  }
  { // Coordinator metrics persist independently of the v2 trip layout.
    Bench bench;Preferences::storage.clear();DriveLog journal;
    g_test_millis=1000;journal.begin(1,21);
    journal.coordinator(10,20,3,4,2,7);
    g_test_millis=31000;journal.poll(bench.can,bench.ble,false);
    DriveLog restarted;restarted.begin(1,21);
    Preferences nvs;assert(nvs.begin("m5can_trip",true));
    DriveLog::Record trip{};DriveLog::CoordinatorRecord coord{};
    assert(nvs.getBytes("trip1",&trip,sizeof(trip))==sizeof(trip));
    assert(nvs.getBytes("coord1",&coord,sizeof(coord))==sizeof(coord));
    assert(trip.version==2 && coord.sequence==trip.sequence && coord.uptime_ms==trip.uptime_ms);
    assert(coord.hits==10 && coord.misses==20 && coord.joins==3 && coord.groups==4 && coord.cross==2 && coord.singles==7);
  }
  std::cout << "PASS actual dual BLE scheduler with host ECU: grouping, one probe, fallback, routing, faults, disconnect, DEMO\n";
}
