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
  static bool fixed(const CanOwner& o){return o.ecu_.fixed();}
  static uint32_t used(CanOwner& o,uint32_t t){o.budget_.refresh(t);return o.budget_.used100;}
  static void prefill(CanOwner& o,uint32_t t){assert(o.budget_.reserve(t,21,120));}
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

static void group(){
 response={};response.status=m5can::DiagnosticStatus::Ok;
 response.completed_at_us=(uint64_t(g_test_millis)+1)*1000;
 response.response_count=1;auto& m=response.responses[0];m.can_id=0x18DAF101;
 const uint8_t d[]={0x41,0x0D,0,0x0C,0x13,0x88,0x05,0x50,0x5B,0x64,0x9A,1,2,3,4,5,6};
 m.length=sizeof(d);std::memcpy(m.data,d,sizeof(d));ready=true;
}
int main(){
 m5can::CanMonitor m;CanOwner o;o.attach(&m);tick(o,1000);good();response.completed_at_us=1010000;response.raw_rx_offsets_us[0]=10000;tick(o,1010);
 // M6: no regular TX before the discovery +200ms boundary.
 const auto afterProbe=requests.size();tick(o,1100);
 assert(requests.size()==afterProbe);
 unsigned groups=0,essential=0;
 for(uint32_t t=1200;t<=4400;t+=100){
   const auto n=requests.size();tick(o,t);
#if defined(M5CAN_SIGNAL_V2_10HZ_LAB)
   const bool due=true;
#else
   const bool due=(t-1200)%200==0;
#endif
   assert(!o.stopped() && requests.size()==n+(due?1:0));
   if(!due)continue;
   const auto& q=requests.back();
   assert(q.can_id==0x18DA01F1 && q.timeout_ms==120);
   const auto acquired=o.samples().acquired;
   const auto sequence=o.samples().values[0].sequence;
   if(q.length==6){++groups;assert(!q.signal_v2_essential);group();}
   else{
     ++essential;assert(q.length==3 && q.signal_v2_essential);
     assert(q.data[0]==1 && q.data[1]==0x0C && q.data[2]==0x0D);good();
   }
   response.completed_at_us=(uint64_t(t)+40)*1000;
   tick(o,t+40);
   assert(!o.stopped() && o.samples().acquired==acquired+1);
   for(unsigned i=0;i<5;++i){
     const auto& s=o.samples().values[i];
     if(q.length==3 && i>=2)assert(s.length==0 && s.quality==8 && s.observed_us==0);
     else assert(s.length && s.quality==0 && s.observed_us==response.completed_at_us);
   }
   assert(o.samples().values[0].sequence==sequence+1);
   assert(o.samples().values[1].sequence==o.samples().values[0].sequence);
 }
#if defined(M5CAN_SIGNAL_V2_10HZ_LAB)
 assert(groups && essential && groups+essential==33 && o.samples().acquired==33);
 std::cout<<"PASS R1 10Hz 3.2s: full="<<groups<<" essential="<<essential<<" samples="<<o.samples().acquired<<"\n";
#else
 assert(groups==17 && essential==0 && o.samples().acquired==17);
 std::cout<<"PASS R1 5Hz 3.2s: full groups every cycle\n";
#endif
 // Essential exhaustion is still a latched STOP with no query or retry.
 CanOwner blocked;blocked.attach(&m);tick(blocked,10000);
 good();response.completed_at_us=10010000;tick(blocked,10010);
 CanOwnerTestAccess::prefill(blocked,10200);
 const auto n=requests.size();tick(blocked,10200);tick(blocked,10400);
 assert(blocked.stopped() && requests.size()==n);
 std::cout<<"PASS R1 essential exhaustion STOP latched, zero submit\n";
}
