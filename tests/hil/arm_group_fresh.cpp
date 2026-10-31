// B-01: compile the real production CanOwner under effective serial-HIL
// PlatformIO -D flags, exercising ARM -> GROUP -> Fresh without physical CAN.
#include "signal_v2_can_owner.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
#ifndef M5CAN_SIGNAL_V2_SERIAL_HIL
#error "B-01 must be compiled as the serial HIL image"
#endif
#ifndef M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL
#error "B-01 must include the production V2 CAN owner"
#endif
#ifdef M5CAN_SIGNAL_V2_STAGE_GATED
#error "B-01 cannot execute a generic ARM under vehicle S1 stage gating"
#endif
#ifdef M5CAN_CAN_TX_HARD_DISABLED
#error "B-01 V2-2 owner must not be hard-disabled at compile time"
#endif

SerialStub Serial;
uint32_t g_test_millis=0;
static std::vector<m5can::DiagnosticRequest> requests;
static m5can::DiagnosticResult response{};
static bool ready=false;
namespace hil {
void note(const char*,...) {}
}
namespace m5can {
CanStats CanMonitor::stats() const { return {}; }
void CanMonitor::revokeLease() {}
bool CanMonitor::acquireLease(uint32_t) { return true; }
bool CanMonitor::submitQuery(const DiagnosticRequest& q) {
  requests.push_back(q); return true;
}
bool CanMonitor::pollQueryResult(DiagnosticResult& r) {
  if (!ready) return false;
  r=response;
  ready=false;
  return true;
}
void CanMonitor::endObservationAudit(ObservationAudit& a,CanStats& s){
  a=audit_;s=stats_;
}
}
static void completeProbe() {
  response={};response.status=m5can::DiagnosticStatus::Ok;
  response.completed_at_us=10000;
  response.response_count=response.raw_frame_count=1;
  auto& p=response.responses[0];p.can_id=0x18DAF101;p.length=6;
  const uint8_t d[]={0x41,0x0C,0x13,0x88,0x0D,0x00};
  std::memcpy(p.data,d,sizeof(d));
  auto& raw=response.raw_frames[0];raw.can_id=p.can_id;raw.dlc=7;
  raw.data[0]=6;std::memcpy(raw.data+1,d,sizeof(d));
  ready=true;
}
static void completeGroup() {
  response={};response.status=m5can::DiagnosticStatus::Ok;
  response.completed_at_us=220000;
  response.response_count=1;
  auto& p=response.responses[0];p.can_id=0x18DAF101;p.length=17;
  const uint8_t d[]={0x41,0x0D,0x00,0x0C,0x13,0x88,0x05,0x80,
                     0x5B,0x7F,0x9A,0x00,0x00,0x4B,0x00,0x00,0x64};
  std::memcpy(p.data,d,sizeof(d));ready=true;
}
int main() {
  static_assert(signal_v2::DiscoveryProfile::kCount==1,"approved RP8 only");
  m5can::CanMonitor m;signal_v2::CanOwner owner;owner.attach(&m);
  signal_v2::Simulator::Demand off{},demand{{200,200,200,200,200}};
  // Initial owner tick, runner's initial STOP, secured USB ARM.
  owner.tick(0,off,false,1);
  owner.tick(1,off,false,2);
  owner.tick(2,demand,true,3);
  assert(!owner.stopped()&&owner.busy()&&requests.size()==1);
  assert(requests.back().can_id==0x18DA01F1&&requests.back().length==3);
  completeProbe();
  owner.tick(12,demand,true,3);
  assert(!owner.stopped()&&!owner.busy());
  owner.tick(203,demand,true,3);
  assert(owner.busy()&&requests.size()==2);
  const auto& q=requests.back();
  assert(q.signal_v2&&q.can_id==0x18DA01F1&&q.length==6);
  const uint8_t expected[]={1,0x0D,0x0C,0x05,0x5B,0x9A};
  assert(std::memcmp(q.data,expected,sizeof(expected))==0);
  // The HIL virtual ECU's FF->FC->CF completion yields a full GROUP PDU.
  completeGroup();
  owner.tick(220,demand,true,3);
  const auto& samples=owner.samples();
  assert(samples.acquired==1&&samples.source_id==0x18DAF101);
  assert(samples.source_generation==4&&samples.mode_epoch==4);
  for (const auto& v:samples.values)
    assert(v.quality==0&&v.observed_us==220000&&v.sequence==1);
  std::cout<<"PASS B-01 serial-HIL ARM -> GROUP -> Fresh 5 PID sample\n";
}
