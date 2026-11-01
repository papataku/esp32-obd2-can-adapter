// Regression adapted from independent reviewer-evidence/owner_lifecycle.cpp.
#include "signal_v2_can_owner.h"
#include <cassert>
#include <cstring>
SerialStub Serial;
uint32_t g_test_millis=0;
static bool available=false;
static m5can::DiagnosticResult response{};
static m5can::DiagnosticRequest submitted{};
static unsigned submits=0,revokes=0;
namespace m5can {
CanStats CanMonitor::stats()const{return {};}
void CanMonitor::endObservationAudit(ObservationAudit& audit,CanStats& out) {
  audit=audit_;audit.owner_confirmed=false;out=stats_;
}
void CanMonitor::revokeAuthorization(uint32_t){revokeLease();}
void CanMonitor::revokeLease(){++revokes;}
bool CanMonitor::acquireLease(uint32_t, uint32_t){return true;}
bool CanMonitor::submitQuery(const DiagnosticRequest &r){submitted=r;++submits;return true;}
bool CanMonitor::pollQueryResult(DiagnosticResult &r){
  if(!available)return false;
  r=response;available=false;return true;
}
}
static void discoveryResult(uint32_t now){
  using namespace m5can;
  response={};response.status=DiagnosticStatus::Ok;response.response_count=1;
  auto &m=response.responses[0];m.can_id=0x18DAF101;m.length=6;
  const uint8_t p[]={0x41,0x0c,0x13,0x88,0x0d,0};std::memcpy(m.data,p,6);
  response.raw_frame_count=1;auto &f=response.raw_frames[0];
  f.can_id=m.can_id;f.dlc=7;f.data[0]=6;std::memcpy(f.data+1,p,6);
  response.completed_at_us=uint64_t(now)*1000;
}
namespace signal_v2 {
struct CanOwnerTestAccess {
  static void verifiedPin(CanOwner &owner){
    // Simulate one RAW-backed bounded physical probe of an approved candidate.
    // This build has no approved profile; production discovery stops (below).
    owner.physical_=false;owner.probe_target_=0x18DAF101;
    discoveryResult(400);
    assert(CanOwner::validProbe(response,0x18DAF101));
    owner.pin(0x18DAF101);
    assert(owner.ecu_.fixed());
  }
};
}
static void physicalResult(){
  response={};response.status=m5can::DiagnosticStatus::Ok;
  response.completed_at_us=1810000;response.response_count=1;
  auto &m=response.responses[0];m.can_id=0x18DAF101;m.length=17;
  const uint8_t group[]={0x41,0x0d,0,0x0c,0x13,0x88,0x05,0x80,0x5b,0x7f,0x9a,0,0,0x4b,0,0,0x64};
  std::memcpy(m.data,group,17);available=true;
}
int main(){
  using namespace signal_v2;
  m5can::CanMonitor monitor;
  Simulator::Demand demand{{200,200,200,200,200}},empty{};
  CanOwner blocked;blocked.attach(&monitor);blocked.tick(200,demand,true,1);
  assert(blocked.stopped() && submits==0); // No unbounded functional broadcast.
  CanOwner owner;owner.attach(&monitor);owner.tick(1,empty,true,1);
  CanOwnerTestAccess::verifiedPin(owner);
  owner.tick(1800,demand,true,1);
  assert(submitted.can_id==0x18DA01F1 && owner.busy());
  const auto gen=owner.samples().source_generation;
  owner.tick(1801,demand,false,2);
  assert(owner.samples().source_generation>gen && owner.busy());
  const auto prior=submits;
  owner.tick(1802,empty,true,3); // Cannot admit anything until cancellation drains.
  assert(submits==prior && owner.busy());
  physicalResult();owner.tick(1820,empty,true,3);
  assert(owner.samples().acquired==0 && owner.samples().values[0].quality>1);
  assert(!owner.busy() && revokes>0);
  // Epoch changes also reject STOP+ARM occurring between two owner ticks.
  CanOwnerTestAccess::verifiedPin(owner);owner.tick(2000,demand,true,3);
  physicalResult();owner.tick(2010,empty,true,5);
  assert(owner.samples().acquired==0 && owner.samples().values[0].quality>1);
  // With the same epoch a real completion is accepted, then three failures latch STOP.
  CanOwnerTestAccess::verifiedPin(owner);owner.tick(2200,demand,true,5);
  physicalResult();owner.tick(2210,demand,true,5);
  assert(owner.samples().acquired==1);
  for(unsigned i=0;i<3;++i){
    owner.tick(2400+i*200,demand,true,5);
    response={};response.status=m5can::DiagnosticStatus::NoData;available=true;
    owner.tick(2410+i*200,demand,true,5);
  }
  assert(owner.stopped());const auto stopped_submits=submits;
  for(unsigned i=0;i<20;++i)owner.tick(3000+i*200,demand,true,5);
  assert(owner.stopped() && submits==stopped_submits);
  // New explicit authorization resets discovery, but cannot evade its load gate.
  owner.tick(7200,demand,false,6);owner.tick(7400,demand,true,7);
  assert(owner.stopped() && submits==stopped_submits);
}
