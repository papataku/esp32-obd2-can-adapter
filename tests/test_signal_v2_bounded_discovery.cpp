// N1 PR-B: evidence-bounded physical ECU discovery (RP8 profile {18DAF101}).
#include "signal_v2_can_owner.h"
#include "diagnostic_transaction.h"
#include <cassert>
#include <cstring>
#include <initializer_list>
#include <iostream>
SerialStub Serial;
uint32_t g_test_millis=0;
static bool available=false;
static m5can::DiagnosticResult response{};
static m5can::DiagnosticRequest submitted{};
static unsigned submits=0,functional=0;
namespace m5can {
CanStats CanMonitor::stats()const{return {};}
void CanMonitor::endObservationAudit(ObservationAudit& audit,CanStats& out) {
  audit=audit_;audit.owner_confirmed=false;out=stats_;
}
void CanMonitor::revokeAuthorization(uint32_t){revokeLease();}
void CanMonitor::revokeLease(){}
bool CanMonitor::acquireLease(uint32_t, uint32_t){return true;}
bool CanMonitor::submitQuery(const DiagnosticRequest &r){
  submitted=r;++submits;functional+=r.can_id==0x18DB33F1U;return true;
}
bool CanMonitor::pollQueryResult(DiagnosticResult &r){
  if(!available)return false;
  r=response;available=false;return true;
}
}
namespace signal_v2 {
struct CanOwnerTestAccess {
  static bool fixed(const CanOwner &o){return o.ecu_.fixed();}
  static uint32_t used100(CanOwner &o,uint32_t now){o.budget_.refresh(now);return o.budget_.used100;}
};
}
using signal_v2::CanOwner;using signal_v2::CanOwnerTestAccess;
static void add(uint32_t id,std::initializer_list<uint8_t> pdu,bool raw=true){
  auto &r=response;auto &m=r.responses[r.response_count++];
  m.can_id=id;m.length=uint8_t(pdu.size());
  std::memcpy(m.data,pdu.begin(),pdu.size());
  if(!raw)return;
  auto &f=r.raw_frames[r.raw_frame_count++];
  f.can_id=id;f.dlc=uint8_t(pdu.size()+1);f.data[0]=uint8_t(pdu.size());
  std::memcpy(f.data+1,pdu.begin(),pdu.size());
}
static void begin(m5can::DiagnosticStatus st=m5can::DiagnosticStatus::Ok){
  response={};response.status=st;response.completed_at_us=1000;available=true;
}
static const signal_v2::Simulator::Demand demand{{200,200,200,200,200}};
// ARM a fresh owner and return it with exactly one probe in flight.
static void armed(CanOwner &o,uint32_t epoch=1){
  o.tick(0,demand,true,epoch);
  assert(o.busy()&&!o.stopped());
  assert(submitted.can_id==0x18DA01F1U && submitted.length==3);
  assert(submitted.data[0]==1&&submitted.data[1]==0x0C&&submitted.data[2]==0x0D);
}
static void expectStop(void(*fill)()){
  m5can::CanMonitor m;CanOwner o;o.attach(&m);armed(o);
  fill();o.tick(10,demand,true,1);
  assert(o.stopped()&&!CanOwnerTestAccess::fixed(o));
  const auto n=submits;
  for(uint32_t t=200;t<4000;t+=200)o.tick(t,demand,true,1);
  assert(submits==n); // latched: no retry, no functional fallback
  assert(o.samples().acquired==0);
}
int main(){
  using m5can::DiagnosticStatus;
  static_assert(signal_v2::DiscoveryProfile::kCount==1,"RP8 profile");
  static_assert(signal_v2::CanBudget::ProbeFrames==4,"PR-A 4-frame reservation");
  {// Combined SF pins the single candidate; then the regular group follows.
    m5can::CanMonitor m;CanOwner o;o.attach(&m);armed(o);
    assert(CanOwnerTestAccess::used100(o,0)==800);
    begin();add(0x18DAF101,{0x41,0x0C,0x13,0x88,0x0D,0x00});
    o.tick(10,demand,true,1);
    assert(CanOwnerTestAccess::fixed(o)&&!o.stopped());
    assert(o.samples().acquired==0); // discovery never publishes a value
    o.tick(200,demand,true,1);
    assert(submitted.can_id==0x18DA01F1U&&submitted.length==6);
  }
  {// Two per-PID SFs in either order are the same contract.
    m5can::CanMonitor m;CanOwner o;o.attach(&m);armed(o);
    begin();add(0x18DAF101,{0x41,0x0D,0x00});add(0x18DAF101,{0x41,0x0C,0x00,0x00});
    o.tick(10,demand,true,1);
    assert(CanOwnerTestAccess::fixed(o)&&!o.stopped());
  }
  {// No responder: not verified, stop without inventing another ECU.
    m5can::CanMonitor m;CanOwner o;o.attach(&m);armed(o);
    begin(DiagnosticStatus::NoData);o.tick(10,demand,true,1);
    assert(o.stopped()&&!CanOwnerTestAccess::fixed(o));
  }
  expectStop([]{begin();add(0x18DAF101,{0x41,0x0C,0x13,0x88,0x0D,0});
                add(0x18DAF101,{0x41,0x0D,0});});                 // extra PDU
  expectStop([]{begin();add(0x18DAF102,{0x41,0x0C,0x13,0x88,0x0D,0});}); // wrong source
  expectStop([]{begin();add(0x18DAF101,{0x41,0x0C,0,0});
                add(0x18DAF101,{0x41,0x0C,0,0});});                // duplicate PID
  expectStop([]{begin();add(0x18DAF101,{0x41,0x0C,0x13,0x88,0x0D,0},false);}); // no RAW
  expectStop([]{begin();add(0x18DAF101,{0x7F,0x01,0x11});});      // negative
  expectStop([]{begin();add(0x18DAF101,{0x41,0x0C,0x13,0x88});});  // speed missing
  expectStop([]{begin(DiagnosticStatus::IsoTpError);});           // FF/CF/overflow
  expectStop([]{begin(DiagnosticStatus::RateLimited);});          // monitor budget
  expectStop([]{begin(DiagnosticStatus::NoData);
                add(0x18DAF101,{0x41,0x0C,0x13,0x88,0x0D,0});});  // late/partial
  {// A probe that never completes hits the 3s deadline and latches STOP.
    m5can::CanMonitor m;CanOwner o;o.attach(&m);armed(o);
    o.tick(2999,demand,true,1);assert(!o.stopped());
    o.tick(3000,demand,true,1);assert(o.stopped());
  }
  {// A new explicit ARM epoch restarts bounded discovery from scratch.
    m5can::CanMonitor m;CanOwner o;o.attach(&m);armed(o);
    begin(DiagnosticStatus::NoData);o.tick(10,demand,true,1);assert(o.stopped());
    o.tick(20,demand,false,2);o.tick(400,demand,true,3);
    assert(!o.stopped()&&submitted.can_id==0x18DA01F1U&&submitted.length==3);
    begin();add(0x18DAF101,{0x41,0x0C,0x13,0x88,0x0D,0});
    o.tick(410,demand,true,3);assert(CanOwnerTestAccess::fixed(o));
  }
  {// Stale completion from a previous epoch can never pin.
    m5can::CanMonitor m;CanOwner o;o.attach(&m);armed(o);
    o.tick(5,demand,true,2); // re-ARM while the probe is outstanding
    begin();add(0x18DAF101,{0x41,0x0C,0x13,0x88,0x0D,0});
    o.tick(10,demand,true,2);assert(!CanOwnerTestAccess::fixed(o));
  }
  assert(functional==0);
  {// Policy: only the approved candidate's physical route is admissible.
    using namespace m5can;
    DiagnosticRequest q{};q.signal_v2=true;q.can_id=0x18DA01F1;q.length=3;
    q.data[0]=1;q.data[1]=0x0C;q.data[2]=0x0D;
    assert(DiagnosticPolicy::approvedPhysicalEcu(0x18DA01F1));
    assert(DiagnosticPolicy::allowedReadRequest(q));
    assert(DiagnosticPolicy::responseBelongsToRequest(q,0x18DAF101));
    assert(!DiagnosticPolicy::responseBelongsToRequest(q,0x18DAF102));
    // Observed-but-unapproved responder (e.g. ECU06) stays forbidden.
    DiagnosticRequest f=q;f.can_id=0x18DB33F1;
    DiagnosticResult r{};r.status=DiagnosticStatus::Ok;r.response_count=1;
    auto &m=r.responses[0];m.can_id=0x18DAF106;m.length=6;
    const uint8_t p[6]={0x41,0x0C,0x13,0x88,0x0D,0x40};std::memcpy(m.data,p,6);
    r.raw_frame_count=1;r.raw_frames[0].can_id=0x18DAF106;r.raw_frames[0].dlc=7;
    r.raw_frames[0].data[0]=6;std::memcpy(r.raw_frames[0].data+1,p,6);
    DiagnosticPolicy::observeFunctionalPairs(f,r);
    q.can_id=0x18DA06F1;
    assert(DiagnosticPolicy::observedPhysicalEcu(q.can_id));
    assert(!DiagnosticPolicy::approvedPhysicalEcu(q.can_id));
    assert(!DiagnosticPolicy::allowedReadRequest(q));
  }
  {// Transaction layer: FF or CF on the probed route fails, never FC.
    using namespace m5can;
    DiagnosticRequest probe{};probe.signal_v2=true;probe.can_id=0x18DA01F1;
    probe.length=3;probe.data[0]=1;probe.data[1]=0x0C;probe.data[2]=0x0D;
    auto frame=[](std::initializer_list<uint8_t> d,uint64_t t){
      TransactionFrame f{};f.can_id=0x18DAF101;f.extended=true;
      f.dlc=uint8_t(d.size());f.timestamp_us=t;std::memcpy(f.data,d.begin(),d.size());return f;};
    DiagnosticTransaction tx;
    tx.begin(probe,1000);
    assert(tx.onFrame(frame({0x10,0x08,0x41,0x0C,0x13,0x88,0x0D,0},2000)).event==
           TransactionEvent::ProtocolError);
    tx.begin(probe,1000);
    assert(tx.onFrame(frame({0x21,0,0,0,0,0,0,0},2000)).event==
           TransactionEvent::ProtocolError);
    tx.begin(probe,1000);
    assert(tx.onFrame(frame({6,0x41,0x0C,0x13,0x88,0x0D,0},2000)).event==
           TransactionEvent::Completed);
    // Third RX frame on the route exceeds the two-SF contract.
    tx.onFrame(frame({6,0x41,0x0C,0x13,0x88,0x0D,0},2100));
    assert(tx.onFrame(frame({6,0x41,0x0C,0x13,0x88,0x0D,0},2200)).event==
           TransactionEvent::ProtocolError);
  }
  std::cout<<"N1 bounded physical discovery PASS\n";
}
