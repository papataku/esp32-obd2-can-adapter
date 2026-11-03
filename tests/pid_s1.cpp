#include "signal_v2_can_owner.h"
#include <cassert>
#include <iostream>
#include <vector>
SerialStub Serial;uint32_t g_test_millis=0;
static bool ready=false,leaseOk=true,submitOk=true;
static m5can::DiagnosticResult response{};
static m5can::CanStats health{};
static std::vector<m5can::DiagnosticRequest> requests;
static std::vector<uint32_t> starts;
namespace m5can {
CanStats CanMonitor::stats()const{return health;}
void CanMonitor::endObservationAudit(ObservationAudit& audit,CanStats& out) {
  audit=audit_;audit.owner_confirmed=false;out=stats_;
}
void CanMonitor::revokeAuthorization(uint32_t){revokeLease();}
void CanMonitor::revokeLease(){}
bool CanMonitor::acquireLease(uint32_t, uint32_t){return leaseOk;}
bool CanMonitor::submitQuery(const DiagnosticRequest& r){requests.push_back(r);starts.push_back(g_test_millis);return submitOk;}
bool CanMonitor::pollQueryResult(DiagnosticResult& r){if(!ready)return false;r=response;ready=false;return true;}
}
namespace signal_v2 {
struct CanOwnerTestAccess {
  static void partial(CanOwner& o,uint32_t t){assert(o.budget_.reserve(t,16,120));}
  static bool fixed(const CanOwner& o){return o.ecu_.fixed();}
  static uint32_t used(CanOwner& o,uint32_t t){o.budget_.refresh(t);return o.budget_.used100;}
  static void prefill(CanOwner& o,uint32_t t){assert(o.budget_.reserve(t,21,120));}
};
}
using namespace signal_v2;
static_assert(PidDiagnostics::kPeriodFloorMs==
#if defined(M5CAN_SIGNAL_V2_10HZ_LAB)
  profiles::kLegacyRp8.period_10hz_ms,
#else
  profiles::kLegacyRp8.period_5hz_ms,
#endif
  "demand floor must equal the owner merge floor");
static Simulator::Demand demand{{100,100,100,100,100}};
static void tick(CanOwner& o,uint32_t t,bool arm=true,uint32_t epoch=1){g_test_millis=t;o.tick(t,demand,arm,epoch);}
#if defined(M5CAN_SIGNAL_V2_RP8_PROFILE)
static void good(){
  response={};response.status=m5can::DiagnosticStatus::Ok;response.completed_at_us=10000;
  response.response_count=response.raw_frame_count=1;
  auto& m=response.responses[0];m.can_id=0x18DAF101;m.length=6;
  const uint8_t d[]={0x41,0x0C,0x13,0x88,0x0D,0};std::memcpy(m.data,d,6);
  auto& f=response.raw_frames[0];f.can_id=m.can_id;f.dlc=7;f.data[0]=6;std::memcpy(f.data+1,d,6);ready=true;
}
#endif

static void group(){
 response={};response.status=m5can::DiagnosticStatus::Ok;
 response.completed_at_us=(uint64_t(g_test_millis)+1)*1000;
 response.response_count=1;auto& m=response.responses[0];m.can_id=0x18DAF101;
 const uint8_t d[]={0x41,0x0D,0,0x0C,0x13,0x88,0x05,0x50,0x5B,0x64,0x9A,1,2,3,4,5,6};
 m.length=sizeof(d);std::memcpy(m.data,d,sizeof(d));ready=true;
}

int main(){
 // Rolling reservations retain the full 120ms receive window at both edges.
 CanBudget boundary;assert(boundary.reserve(1000,12,120));
 boundary.refresh(1219);assert(boundary.used100==2400);
 boundary.refresh(1220);assert(boundary.used100==0&&boundary.used1000==2400);
 boundary.refresh(2119);assert(boundary.used1000==2400);
 boundary.refresh(2120);assert(boundary.used1000==0);
 m5can::CanMonitor m;CanOwner o;o.attach(&m);
 tick(o,1000);good();tick(o,1010);
 assert(o.pidDiagnostics().pid[0].completed==0); // probe is not a gauge
 tick(o,1200);assert(o.busy());
 // Full 120ms response delay: busy ticks must remain in demand denominator.
 tick(o,1319);assert(o.pidDiagnostics().pid[0].completed==0);
 group();response.completed_at_us=1320000;tick(o,1320);
 for(const auto& c:o.pidDiagnostics().pid){assert(c.demand==2&&c.admitted==1&&c.completed==1);}
 tick(o,1321);assert(o.pidDiagnostics().pid[0].completed==1); // no snapshot/tick duplicate
 // Missed scheduler ticks catch up demand independently of actual admission.
 tick(o,2000);
 assert(o.pidDiagnostics().pid[0].demand==6);
 assert(o.pidDiagnostics().pid[0].admitted==2);
 const auto n=requests.size();tick(o,2001,false,2);
 group();tick(o,2120,false,2);tick(o,3000,false,2);
 assert(requests.size()==n);
 for(const auto& c:o.pidDiagnostics().pid){assert(c.cancelled==1&&c.completed==1&&c.demand==6);}
 // Concurrent PID subscriptions merge into one fastest group demand stream.
 demand={{200,400,1000,500,800}};
 CanOwner small;small.attach(&m);tick(small,10000);good();tick(small,10010);
 assert(CanOwnerTestAccess::fixed(small));
 // 16 frames + probe(4) leaves room for essential(4), but not full(12).
 assert(CanOwnerTestAccess::used(small,10200)==800);
 // Test access uses production reservation, not a scheduler mutation.
 // Direct access through the friend fixture below.
 CanOwnerTestAccess::partial(small,10200);
 tick(small,10200);assert(requests.back().signal_v2_essential);
 good();response.completed_at_us=10320000;tick(small,10320);
 for(unsigned i=0;i<5;++i){
   const auto& c=small.pidDiagnostics().pid[i];
   assert(c.demand==2&&c.admitted==(i<2?1U:0U)&&c.completed==(i<2?1U:0U));
   assert(c.shed==(i<2?0U:1U));
 }
 // Read-out does not alter counters; successful output is not another completion.
 for(unsigned k=0;k<20;++k)(void)small.pidDiagnostics();
 assert(small.pidDiagnostics().pid[2].completed==0);
 // RX overrun stops before consuming a pending completion; zero later TX.
 health.driver_rx_overrun=1;const auto before=requests.size();tick(small,10400);
 const auto active_at_stop=small.pidDiagnostics().active_ms;
 tick(small,10600);assert(small.pidDiagnostics().active_ms==active_at_stop);assert(small.stopped()&&requests.size()==before);health={};
 // Unknown submission outcome cannot be retried by the owner.
 CanOwner uncertain;uncertain.attach(&m);tick(uncertain,20000);good();tick(uncertain,20010);
 submitOk=false;const auto attempts=requests.size();tick(uncertain,20200);tick(uncertain,20400);
 assert(uncertain.stopped()&&requests.size()==attempts+1);submitOk=true;
 for(const auto& c:uncertain.pidDiagnostics().pid)assert(c.admitted==0&&c.completed==0&&c.denied==1);
 // Valid admission but invalid response is failure, never completion.
 CanOwner bad;bad.attach(&m);tick(bad,30000);good();tick(bad,30010);tick(bad,30200);
 group();response.responses[0].data[0]=0x7f;tick(bad,30320);
 for(const auto& c:bad.pidDiagnostics().pid)assert(c.admitted==1&&c.failed==1&&c.completed==0);
 // Both reservations exhausted: auxiliary shed and essential denial remain distinct.
 CanOwner denied;denied.attach(&m);tick(denied,40000);good();tick(denied,40010);
 CanOwnerTestAccess::prefill(denied,40200);const auto blocked=requests.size();
 tick(denied,40200);tick(denied,40400);
 assert(denied.stopped()&&requests.size()==blocked);
 for(unsigned i=0;i<5;++i){const auto& c=denied.pidDiagnostics().pid[i];
   assert(c.admitted==0&&c.completed==0&&c.denied==(i<2?1U:0U)&&c.shed==(i<2?0U:1U));}
 // Overrun while a completion waits cannot promote it to a new gauge sample.
 CanOwner loss;loss.attach(&m);tick(loss,50000);good();tick(loss,50010);tick(loss,50200);
 group();health.driver_rx_overrun=1;tick(loss,50320);health={};
 for(const auto& c:loss.pidDiagnostics().pid)assert(c.cancelled==1&&c.completed==0);
 // FC: a multi-frame group (FF+2CF RAW, one monitor FC) is one instrument
 // admission and exactly one completion; the FC is not an owner request.
 CanOwner fc;fc.attach(&m);tick(fc,60000);good();tick(fc,60010);tick(fc,60200);
 const auto fc_tx=requests.size();
 group();response.raw_frame_count=3;health.signal_v2_fc=1;health.signal_v2_rx=3;
 tick(fc,60320);tick(fc,60321);
 assert(requests.size()==fc_tx);
 for(const auto& c:fc.pidDiagnostics().pid)assert(c.admitted==1&&c.completed==1&&c.failed==0);
 // All-RX: unrelated bus traffic charged by the monitor never changes demand,
 // admission or completion; only the owner cadence does.
 health.rx_frames=100000;health.signal_v2_rx=50000;
 const auto fc_before=fc.pidDiagnostics();
 tick(fc,60322);tick(fc,60399);
 for(unsigned i=0;i<5;++i){
   assert(fc.pidDiagnostics().pid[i].demand==fc_before.pid[i].demand);
   assert(fc.pidDiagnostics().pid[i].completed==fc_before.pid[i].completed);
 }
 health={};
 // TX=0 after STOP: 100 more armed ticks (5s) after a fault STOP send nothing,
 // and the closed window accrues neither demand nor active time.
 health.driver_rx_missed=1;tick(fc,60400);health={};
 assert(fc.stopped());
 const auto stop_tx=requests.size();const auto at_stop=fc.pidDiagnostics();
 for(uint32_t t=60450;t<65450;t+=50){tick(fc,t);if(t%200==0){good();}}
 ready=false;
 assert(requests.size()==stop_tx);
 assert(fc.pidDiagnostics().active_ms==at_stop.active_ms);
 for(unsigned i=0;i<5;++i)assert(fc.pidDiagnostics().pid[i].demand==at_stop.pid[i].demand);
 // Saturation must not wrap a regressed counter to a deceptively small value.
 PidDiagnostics d;d.pid[0].completed=UINT64_MAX;d.admit(false);d.finish(true);
 assert(d.pid[0].completed==UINT64_MAX);
 std::cout<<"PASS PID S1 120ms/rolling boundary/busy/missed due/concurrent demand/shed/FC/all-RX/overrun/uncertain/STOP TX0/invalid/saturation\n";
}
