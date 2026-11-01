#include "signal_v2_can_owner.h"
#include <cassert>
#include <iostream>
#include <vector>
#include <string>
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
  static bool fixed(const CanOwner& o){return o.ecu_.fixed();}
  static uint32_t used(CanOwner& o,uint32_t t){o.budget_.refresh(t);return o.budget_.used100;}
  static void prefill(CanOwner& o){assert(o.budget_.reserve(0,24,120));}
};
}
using namespace signal_v2;
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
int main(int argc,char** argv){
  const std::string selected=argc>1?argv[1]:"all";
  auto test=[&](const char* id){return selected=="all"||selected==id;};
  (void)test;
  m5can::CanMonitor m;
#if !defined(M5CAN_SIGNAL_V2_RP8_PROFILE)
  CanOwner o;o.attach(&m);auto n=requests.size();tick(o,0);
  assert(o.stopped()&&requests.size()==n);std::cout<<"PASS empty profile STOP zero TX\n";
#else
  if(test("controls")){CanOwner o;o.attach(&m);CanOwnerTestAccess::prefill(o);auto n=requests.size();tick(o,0);
   assert(o.stopped()&&n==requests.size());std::cout<<"PASS upper budget denies entire 800-bit probe, zero submit\n";}
  if(test("M6")){CanOwner o;o.attach(&m);tick(o,0);assert(CanOwnerTestAccess::used(o,0)==800);
   good();tick(o,10);assert(CanOwnerTestAccess::fixed(o));
   auto n=requests.size();tick(o,100);
#if !defined(M5CAN_SIGNAL_V2_10HZ_LAB)
   assert(requests.size()==n);tick(o,200);
   assert(requests.back().length==6);std::cout<<"PASS 5Hz probe -> group at 200ms\n";
#else
   assert(requests.size()==n);tick(o,200);assert(requests.size()==n+1);std::cout<<"PASS M6 10Hz waits 200ms after probe\n";
#endif
  }
  if(test("M5")){CanOwner o;o.attach(&m);submitOk=false;auto n=requests.size();tick(o,1000);tick(o,1200);
   assert(o.stopped()&&requests.size()==n+1);submitOk=true;
   std::cout<<"PASS M5 rejected submit latches STOP, one attempt\n";}
  if(test("M5")){CanOwner o;o.attach(&m);leaseOk=false;const auto n=requests.size();
    tick(o,1500);tick(o,1700);assert(o.stopped()&&requests.size()==n);leaseOk=true;
    std::cout<<"PASS M5 failed lease latches STOP with zero submissions\n";
  }
  if(test("M4")){CanOwner o;o.attach(&m);tick(o,2000);good();response.completed_at_us=2010000;tick(o,5000);
   assert(!CanOwnerTestAccess::fixed(o)&&o.stopped());
   std::cout<<"PASS M4 queued completion at deadline cannot pin\n";}
  if(test("controls")){CanOwner o;o.attach(&m);tick(o,6000);tick(o,9000);assert(o.stopped());
   good();tick(o,9010);assert(!CanOwnerTestAccess::fixed(o));
   auto n=requests.size();tick(o,9200);assert(requests.size()==n);
   std::cout<<"PASS missing completion 3s STOP latched; subsequent completion discarded\n";}
  if(test("controls")){CanOwner o;o.attach(&m);tick(o,10000);tick(o,10001,true,2);good();tick(o,10010,true,2);
   assert(!CanOwnerTestAccess::fixed(o));
   good();tick(o,10020,true,2);assert(CanOwnerTestAccess::fixed(o));
   std::cout<<"PASS old-epoch completion cannot pin; new-epoch result pins\n";}
  if(test("controls")){CanOwner o;o.attach(&m);tick(o,11000);good();response.status=m5can::DiagnosticStatus::NoData;
   tick(o,11010);assert(o.stopped());auto n=requests.size();tick(o,11200);assert(n==requests.size());
   std::cout<<"PASS NoData-with-frames STOP latched\n";}
  if(test("controls")){CanOwner o;o.attach(&m);tick(o,12000);good();response.raw_rx_offsets_us[0]=120001;
   tick(o,12010);assert(!CanOwnerTestAccess::fixed(o)&&o.stopped());
   std::cout<<"PASS out-of-window RAW offsets rejected at owner too\n";}
  if(test("M7")){CanOwner o;o.attach(&m);tick(o,13000);health.driver_rx_missed=health.driver_rx_overrun=1;
   good();tick(o,13010);assert(!CanOwnerTestAccess::fixed(o)&&o.stopped());
   auto n=requests.size();tick(o,13200);assert(requests.size()==n);
   health={};std::cout<<"PASS M7 RX loss stops before pin and blocks next group\n";}
  for(const auto& q:requests)assert(q.can_id==0x18DA01F1);
#endif
}
