#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
#include <Preferences.h>
#include "drive_log.h"
#include "ble_evidence_journal.h"
#include "evidence_store.h"
using namespace m5can;
struct CombinedFlash final:EvidenceFlash {
  std::vector<uint8_t> data=std::vector<uint8_t>(384*4096,0xFF);
  uint32_t size() const override{return data.size();}
  bool read(uint32_t o,void* p,size_t n) override {
    if(o>data.size() || n>data.size()-o)return false;
    std::memcpy(p,data.data()+o,n);return true;
  }
  bool write(uint32_t o,const void* p,size_t n) override {
    if(o>data.size() || n>data.size()-o)return false;
    const auto* b=static_cast<const uint8_t*>(p);
    for(size_t i=0;i<n;++i) {
      if((data[o+i]&b[i])!=b[i])return false;
      data[o+i]&=b[i];
    }
    return true;
  }
  bool erase(uint32_t o) override {
    if(o%4096 || o+4096>data.size())return false;
    std::memset(data.data()+o,0xFF,4096);return true;
  }
};
namespace m5can {
CanStats CanMonitor::stats() const{return stats_;}
void CanMonitor::stats(CanStats& copy) const{copy=stats_;}
uint32_t CanMonitor::leaseRemainingMs() const{return 0;}
}
int main(){
  CombinedFlash flash;assert(evidenceStore().begin(flash,true));
  Preferences::storage.clear();Preferences::writes=0;
  const std::vector<uint8_t> settings={1,4,9,16};
  Preferences::storage["m5can_cfg/setting"]=settings;
  CanMonitor can;BleElmTransport ble;
  for(unsigned boot=1;boot<=64;++boot) {
    g_test_millis=boot*40000;
    DriveLog journal;journal.begin(1);
    assert(journal.snapshot().sequence==boot);
    DiagnosticResult result{};
    result.status=DiagnosticStatus::Ok;result.response_count=5;
    result.raw_frame_count=48;result.transaction_us=10000;
    for(unsigned i=0;i<48;++i) {
      result.raw_frames[i].can_id=0x18DAF101U+(i%5);
      result.raw_frames[i].dlc=8;
      for(unsigned b=0;b<8;++b)result.raw_frames[i].data[b]=boot+i+b;
      result.raw_rx_offsets_us[i]=656+i*137;
    }
    journal.recordFirstProbe(7,result,0x18DAF101U,0,0);
    result.status=DiagnosticStatus::NoData;
    journal.recordFirstFailure(7,result,0x18DAF101U,1,5);
    journal.poll(can,ble,false);
    assert(journal.probeSaved());
    BleEvidenceJournal link;link.begin(boot,"host-combined");
    BleDiagnosticEvent event{};
    event.kind=BleDiagnosticEvent::Kind::Disconnect;
    event.sequence=1;event.reason_known=true;event.reason=520;
    event.connection_id=7;event.address_suffix=0xDD41;event.generation=boot;
    link.append(event,0);assert(link.poll(can) && link.saved());
    assert(Preferences::storage.at("m5can_cfg/setting")==settings);
    assert(Preferences::storage.count("m5can_trip/probe0")==0);
    assert(Preferences::storage.count("m5can_blelog/ble0")==0);
  }
  assert(Preferences::storage.size()==9); // eight legacy trips + settings
  assert(evidenceStore().begin(flash,false)); // actual flash-index reconstruction
  for(unsigned boot=57;boot<=64;++boot) {
    DriveLog::ProbeRecord probe{},failure{};
    assert(evidenceStore().get(EvidenceStore::Probe,boot%8,&probe,sizeof(probe))==sizeof(probe));
    assert(evidenceStore().get(EvidenceStore::Failure,boot%8,&failure,sizeof(failure))==sizeof(failure));
    assert(probe.sequence==boot && probe.raw_saved==48 && failure.sequence==boot);
    for(unsigned i=0;i<48;++i) {
      assert(probe.rx_offset_us[i]==656+i*137);
      for(unsigned b=0;b<8;++b)assert(probe.raw[i].data[b]==static_cast<uint8_t>(boot+i+b));
    }
  }
  for(unsigned boot=61;boot<=64;++boot) {
    BleEvidenceJournal::Record record{};
    assert(evidenceStore().get(EvidenceStore::Ble,boot%4,&record,sizeof(record))==sizeof(record));
    assert(record.boot==boot && record.events[0].reason==520);
  }
  std::cout<<"PASS combined64 boots:48 original CAN frames/RX offsets, separate first failure, BLE reasons, legacy trip/settings preservation\n";
}
