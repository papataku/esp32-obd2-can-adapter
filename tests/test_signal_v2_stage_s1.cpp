// PR-C2: stage S1 = one physical 0C/0D observation (RP8 profile {18DAF101}).
#include "signal_v2_can_owner.h"
#include "diagnostic_transaction.h"
#include <cassert>
#include <cstring>
#include <initializer_list>
#include <iostream>
SerialStub Serial;
uint32_t g_test_millis=0;
static bool available=false,admit=true;
static m5can::DiagnosticResult response{};
static m5can::DiagnosticRequest submitted{};
static unsigned submits=0,functional=0;
static m5can::CanStats health{};
namespace m5can {
CanStats CanMonitor::stats()const{return health;}
void CanMonitor::revokeLease(){}
bool CanMonitor::acquireLease(uint32_t){return true;}
bool CanMonitor::submitQuery(const DiagnosticRequest &r){
  if(!admit)return false;
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
  static void rx(m5can::CanMonitor &m,uint32_t id,std::initializer_list<uint8_t> d){
    uint8_t b[8]{};std::memcpy(b,d.begin(),d.size());
    m.audit_.note(id,true,false,uint8_t(d.size()),b);
  }
  static void counters(m5can::CanMonitor &m,const m5can::CanStats &s){m.stats_=s;}
};
}
using signal_v2::CanOwner;using signal_v2::CanOwnerTestAccess;
using m5can::DiagnosticStatus;
static void add(uint32_t id,std::initializer_list<uint8_t> pdu){
  auto &r=response;auto &m=r.responses[r.response_count++];
  m.can_id=id;m.length=uint8_t(pdu.size());
  std::memcpy(m.data,pdu.begin(),pdu.size());
  auto &f=r.raw_frames[r.raw_frame_count++];
  f.can_id=id;f.dlc=uint8_t(pdu.size()+1);f.data[0]=uint8_t(pdu.size());
  std::memcpy(f.data+1,pdu.begin(),pdu.size());
}
// The probe was actually submitted to the driver at tx_ms.
static void begin(DiagnosticStatus st=DiagnosticStatus::Ok,uint32_t tx_ms=5){
  response={};response.status=st;response.completed_at_us=1000;available=true;
  response.request_tx_us=uint64_t(tx_ms)*1000ULL;
}
static const signal_v2::Simulator::Demand none{};
static const signal_v2::Simulator::Demand demand{{200,200,200,200,200}};
static std::string record(){
  const auto &l=SerialStub::lines;
  const auto at=l.rfind("#SIGNALV2_S1,");
  return at==std::string::npos?std::string():l.substr(at,l.find('\n',at)-at);
}
// Request S1 for the next epoch; it needs no BLE demand.
static void s1(CanOwner &o,uint32_t now=0,uint32_t epoch=1){
  SerialStub::lines.clear();
  o.requestStageS1(epoch);o.tick(now,none,true,epoch);
  assert(o.stageS1()&&o.busy()&&!o.stopped());
  assert(submitted.can_id==0x18DA01F1U&&submitted.length==3);
  assert(submitted.data[0]==1&&submitted.data[1]==0x0C&&submitted.data[2]==0x0D);
}
// After the probe, nothing else is ever submitted and output stays Invalid.
// The 1,000 ms window counts from the actual TX (t=5), not the enqueue.
static void quietUntilStop(CanOwner &o,unsigned n){
  for(uint32_t t=20;t<1005;t+=50){o.tick(t,demand,true,1);assert(!o.stopped());}
  o.tick(1004,demand,true,1);assert(!o.stopped());
  o.tick(1005,demand,true,1);assert(o.stopped());
  for(uint32_t t=1000;t<5000;t+=100)o.tick(t,demand,true,1);
  assert(submits==n&&o.samples().acquired==0&&!CanOwnerTestAccess::fixed(o));
}
static void expectContractStop(void(*fill)(),const char* reason){
  m5can::CanMonitor m;CanOwner o;o.attach(&m);s1(o);
  const auto n=submits;
  fill();o.tick(10,demand,true,1);
  assert(o.stopped());
  for(uint32_t t=20;t<5000;t+=100)o.tick(t,demand,true,1);
  assert(submits==n&&o.samples().acquired==0&&!CanOwnerTestAccess::fixed(o));
  const auto r=record();
  assert(r.find(std::string("stop=")+reason)!=std::string::npos);
  assert(r.find("complete=0")!=std::string::npos);
}
int main(){
  SerialStub::capture=true;
  static_assert(signal_v2::DiscoveryProfile::kCount==1,"RP8 profile");
  {// Generic ARM never enters S1.
    m5can::CanMonitor m;CanOwner o;o.attach(&m);
    o.tick(0,none,true,1);
    assert(!o.stageS1()&&!o.busy()); // ARM without demand does nothing
  }
  {// Combined SF: recorded, never pinned, never followed by a group request.
    m5can::CanMonitor m;CanOwner o;o.attach(&m);s1(o);
    const auto n=submits;
    begin();add(0x18DAF101,{0x41,0x0C,0x13,0x88,0x0D,0x00});
    CanOwnerTestAccess::rx(m,0x18DAF101,{6,0x41,0x0C,0x13,0x88,0x0D,0x00});
    CanOwnerTestAccess::rx(m,0x0CF00400,{0xFF,0,0,0,0,0,0,0}); // ambient traffic
    o.tick(10,demand,true,1);
    assert(!o.stopped()&&!CanOwnerTestAccess::fixed(o));
    quietUntilStop(o,n);
    const auto r=record();
    assert(r.find("requests=1")!=std::string::npos&&r.find("fc=0")!=std::string::npos);
    assert(r.find("outcome=SINGLE_FRAME_CONTRACT")!=std::string::npos);
    assert(r.find("stop=S1_OBSERVATION_COMPLETE")!=std::string::npos);
    assert(r.find("all_rx=2")!=std::string::npos&&r.find("complete=1")!=std::string::npos);
    assert(SerialStub::lines.find("#SIGNALV2_S1_ID,epoch=1,id=18DAF101,ext=1,sf=1,ff=0")!=std::string::npos);
    assert(SerialStub::lines.find("id=0CF00400,ext=1,sf=0,ff=0,cf=0,fc=0,other=1")!=std::string::npos);
  }
  {// Two per-PID SFs are the same contract.
    m5can::CanMonitor m;CanOwner o;o.attach(&m);s1(o);
    const auto n=submits;
    begin();add(0x18DAF101,{0x41,0x0D,0x00});add(0x18DAF101,{0x41,0x0C,0x13,0x88});
    o.tick(10,demand,true,1);quietUntilStop(o,n);
    assert(record().find("outcome=SINGLE_FRAME_CONTRACT")!=std::string::npos);
  }
  {// No response is an observation result, not a reason to retry.
    m5can::CanMonitor m;CanOwner o;o.attach(&m);s1(o);
    const auto n=submits;
    begin(DiagnosticStatus::NoData);o.tick(10,demand,true,1);
    quietUntilStop(o,n);
    assert(record().find("outcome=NO_RESPONSE")!=std::string::npos);
  }
  expectContractStop([]{begin();add(0x18DAF101,{0x41,0x0C,0x13,0x88,0x0D,0});
                        add(0x18DAF101,{0x41,0x0D,0});},"DISCOVERY_RESPONSE_CONTRACT"); // 3rd
  expectContractStop([]{begin();add(0x18DAF102,{0x41,0x0C,0x13,0x88,0x0D,0});},
                     "DISCOVERY_RESPONSE_CONTRACT");                                   // other source
  expectContractStop([]{begin(DiagnosticStatus::IsoTpError);},"DISCOVERY_RESPONSE_CONTRACT"); // FF/CF
  expectContractStop([]{begin(DiagnosticStatus::NoData);
                        add(0x18DAF101,{0x41,0x0C,0x13,0x88,0x0D,0});},"DISCOVERY_RESPONSE_CONTRACT"); // late
  {// RX loss during the window stops and rejects the evidence.
    m5can::CanMonitor m;CanOwner o;o.attach(&m);s1(o);
    begin();add(0x18DAF101,{0x41,0x0C,0x13,0x88,0x0D,0});o.tick(10,demand,true,1);
    health.driver_rx_missed=1;o.tick(300,demand,true,1);health={};
    assert(o.stopped());
    const auto r=record();
    assert(r.find("stop=RX_ACCOUNTING_OR_BUS_FAULT")!=std::string::npos&&r.find("complete=0")!=std::string::npos);
  }
  {// Admission failure: no retry.
    m5can::CanMonitor m;CanOwner o;o.attach(&m);
    admit=false;o.requestStageS1(1);o.tick(0,none,true,1);admit=true;
    const auto n=submits;
    for(uint32_t t=0;t<3000;t+=100)o.tick(t,none,true,1);
    assert(o.stopped()&&submits==n);
    assert(record().find("stop=QUERY_ADMISSION_FAILED")!=std::string::npos);
  }
  {// Operator STOP mid-window still emits the record; the next generic ARM
   // is a new epoch that does not reuse anything from S1.
    m5can::CanMonitor m;CanOwner o;o.attach(&m);s1(o);
    begin();add(0x18DAF101,{0x41,0x0C,0x13,0x88,0x0D,0});o.tick(10,demand,true,1);
    o.noteAuthorizationEnd("OPERATOR_STOP");o.tick(100,none,false,2);
    // The record keeps the observed epoch and the actual cause.
    assert(record().find("#SIGNALV2_S1,epoch=1,")!=std::string::npos);
    assert(record().find("stop=OPERATOR_STOP")!=std::string::npos);
    o.tick(400,demand,true,3);
    assert(!o.stageS1()&&!CanOwnerTestAccess::fixed(o));
    assert(submitted.length==3); // fresh bounded discovery, not a group read
  }
  {// F1: a request bound to epoch 2 is never consumed by a later epoch.
    m5can::CanMonitor m;CanOwner o;o.attach(&m);
    const auto n=submits;
    o.requestStageS1(2);
    // The disconnect bumped the epoch before the S1 arm landed; the owner
    // first observes a later armed epoch, which must not become S1.
    o.tick(10,none,true,4);
    assert(!o.stageS1()&&submits==n);
  }
  {// F2: enqueue at t=0 but the driver transmits at t=200: the window ends
   // at 1,200, never at 1,000.
    m5can::CanMonitor m;CanOwner o;o.attach(&m);s1(o);
    begin(DiagnosticStatus::NoData,200);o.tick(210,demand,true,1);
    o.tick(1000,demand,true,1);assert(!o.stopped());
    o.tick(1199,demand,true,1);assert(!o.stopped());
    o.tick(1200,demand,true,1);assert(o.stopped());
    assert(record().find("request_ms=200")!=std::string::npos);
  }
  {// F2: no completion within 1 s stops early as incomplete evidence.
    m5can::CanMonitor m;CanOwner o;o.attach(&m);s1(o);
    o.tick(999,demand,true,1);assert(!o.stopped());
    o.tick(1000,demand,true,1);assert(o.stopped());
    const auto r=record();
    assert(r.find("stop=S1_NO_COMPLETION")!=std::string::npos&&r.find("complete=0")!=std::string::npos);
  }
  {// A completion that never transmitted is not an observation.
    m5can::CanMonitor m;CanOwner o;o.attach(&m);s1(o);
    begin(DiagnosticStatus::DriverError,0);o.tick(10,demand,true,1);
    assert(o.stopped()&&record().find("stop=S1_NOT_TRANSMITTED")!=std::string::npos);
  }
  {// F4: counters and per-ID audit share one boundary; a bus error inside
   // the window makes the evidence incomplete.
    m5can::CanMonitor m;CanOwner o;o.attach(&m);s1(o);
    begin();add(0x18DAF101,{0x41,0x0C,0x13,0x88,0x0D,0});o.tick(10,demand,true,1);
    m5can::CanStats s{};s.driver_bus_error=1;CanOwnerTestAccess::counters(m,s);
    o.tick(1005,demand,true,1);assert(o.stopped());
    const auto r=record();
    assert(r.find("bus_error=1")!=std::string::npos&&r.find("complete=0")!=std::string::npos);
  }
  {// F5: per-ID capacity overflow stops immediately.
    m5can::CanMonitor m;CanOwner o;o.attach(&m);s1(o);
    begin(DiagnosticStatus::NoData);o.tick(10,demand,true,1);
    for(uint32_t id=0x100;id<0x109;++id)CanOwnerTestAccess::rx(m,id,{0xFF});
    o.tick(20,demand,true,1);assert(o.stopped());
    assert(record().find("stop=S1_AUDIT_OVERFLOW")!=std::string::npos);
  }
  assert(functional==0);
  std::cout<<"PR-C2 stage S1 single observation PASS\n";
}
