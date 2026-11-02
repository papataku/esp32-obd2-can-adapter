#include "signal_v2_can_owner.h"
#include <cassert>
#include <iostream>
#include <vector>
SerialStub Serial;
uint32_t g_test_millis=1;
namespace signal_v2 {
struct CanOwnerTestAccess {
  static void service(m5can::CanMonitor& m) {
    m5can::DiagnosticRequest q{};
    if(xQueueReceive(m.query_queue_,&q,0)==pdTRUE){
      m5can::DiagnosticResult r{};m.performQuery(q,r);
      assert(xQueueSend(m.result_queue_,&r,0)==pdTRUE);
    }
  }
  static CanBudget& ownerBudget(CanOwner& o){return o.budget_;}
  static CanBudget& lowerBudget(m5can::CanMonitor& m){return m.signal_budget_;}
  static bool second(m5can::CanMonitor& m,uint32_t epoch) {
    m5can::DiagnosticRequest q{};q.signal_v2=true;q.authorization_epoch=epoch;
    q.can_id=0x7DF;q.length=2;q.data[0]=1;q.timeout_ms=120;
    return m.submitQuery(q);
  }
  static int again(m5can::CanMonitor& m) {
    twai_message_t tx{};tx.identifier=0x7DF;tx.ss=1;tx.data_length_code=8;
    std::memcpy(tx.data,signal_v2::Discovery11::Request,8);
    return m.transmitOwnedFrame(tx,20,1);
  }
  static int fc(m5can::CanMonitor& m,uint32_t epoch) {
    return m.transmitOwnedFrame(m.makeFlowControlFrame(0x7E0),20,epoch);
  }
  static void capture(m5can::CanMonitor& m,const twai_message_t& f){m.captureMessage(f,uint64_t(millis())*1000);}
};
}
using signal_v2::CanOwnerTestAccess;
static m5can::CanMonitor* monitor=nullptr;
static std::string scenario;
static bool injected=false;
static uint32_t tx_at=0;
namespace hil {void admissionHook(){}}
static void inject(const std::string& body,uint32_t gen=0) {
  const auto cmd="HIL RX "+std::to_string(gen?gen:hil::currentGeneration())+" "+body;
  assert(hil::usbCommand(cmd.c_str()));
}
// Host stub time hook: commands enter the real serial TWAI bridge while the
// production owner is waiting. No physical device or serial port is opened.
void p5TimeHook() {
  if(!monitor)return;
  for(unsigned i=0;i<8;++i)hil::poll();
  if(!monitor->stats().tx_success)return;
  if(!tx_at)tx_at=millis();
  if(injected || millis()-tx_at<(scenario=="late"?150U:10U))return;
  injected=true;
  if(scenario=="reentry")assert(CanOwnerTestAccess::again(*monitor)!=ESP_OK);
  if(scenario=="epoch" || scenario=="profile")monitor->revokeAuthorizationAsync(2);
  else if(scenario=="timeout" || scenario=="fc"){}
  else if(scenario=="buserr")hil::usbCommand("HIL BUSERR 1");
  else if(scenario=="drop")hil::usbCommand("HIL RXMISS 1");
  else if(scenario=="busoff")hil::usbCommand("HIL BUSOFF");
  else if(scenario=="overrun")for(unsigned i=0;i<257;++i)inject("7E8 0 0641001234567800");
  else if(scenario=="old")inject("7E8 0 0641001234567800",hil::currentGeneration()-1);
  else if(scenario=="ff")inject("7E8 0 1010410012345600");
  else if(scenario=="cf")inject("7E8 0 2141001234567800");
  else if(scenario=="rxfc")inject("7E8 0 3000000000000000");
  else if(scenario=="range")inject("7E7 0 0641001234567800");
  else if(scenario=="29bit")inject("18DAF101 1 0641001234567800");
  else {
    unsigned n=(scenario=="eight" || scenario=="ninth")?8:1;
    for(unsigned i=0;i<n;++i) {
      char frame[80];std::snprintf(frame,sizeof(frame),"%03X 0 0641001234567800",0x7EF-i);
      inject(frame);
    }
    if(scenario=="duplicate" || scenario=="ninth")inject("7EF 0 0641001234567800");
  }
}
int main(int argc,char** argv) {
  assert(argc==2);scenario=argv[1];SerialStub::capture=true;
  using D=signal_v2::Discovery11;
  const uint8_t exact[]={2,1,0,0,0,0,0,0};assert(std::memcmp(D::Request,exact,8)==0);
  if(scenario=="limits") {
    D d;d.receiving=true;d.tx_us=1000;d.count=8;
    assert(!d.receive(0x7E8,false,false,false,8,(const uint8_t*)"\006\101\00012345",2000));
    d=D{};d.receiving=true;d.tx_us=1000;d.rx=16;
    assert(!d.receive(0x7E8,false,false,false,8,(const uint8_t*)"\006\101\00012345",2000));
    signal_v2::CanBudget b;
    assert(b.reserve(1,17,1000));assert(b.used100==3400 && b.used1000==3400);
    assert(!b.reserve(1,17,1000));assert(!b.reserve(100,17,1000));
    assert(b.reserve(1101,17,1000));
    signal_v2::CanBudget one;
    for(unsigned i=0;i<5;++i)assert(one.reserve(i*110,25));
    assert(!one.reserve(550,1));assert(one.reserve(1001,1));
    std::cout<<"PASS limits ninth/17RX and rolling 100ms/1s\n";return 0;
  }
  m5can::CanMonitor m;monitor=&m;assert(m.begin());
  signal_v2::CanOwner o;o.attach(&m);const signal_v2::Simulator::Demand demand{{200,200,200,200,200}};
  assert(!o.armDiscovery11(millis(),1,true));
  assert(o.armDiscovery11(millis(),1,false));
  assert(!o.armDiscovery11(millis(),1,false));
  if(scenario=="ownerbudget")assert(CanOwnerTestAccess::ownerBudget(o).reserve(millis(),9,1000));
  if(scenario=="lowerbudget")assert(CanOwnerTestAccess::lowerBudget(m).reserve(millis(),9,1000));
  o.tick(millis(),demand,true,1);
  if(scenario=="double-entry")assert(!CanOwnerTestAccess::second(m,1));
  if(scenario=="txfail")hil::usbCommand("HIL TXFAIL 1");
  if(scenario=="fc") {
    assert(CanOwnerTestAccess::fc(m,1)!=ESP_OK);assert(m.stats().tx_attempts==0);
  }
  if(scenario=="deadline")g_test_millis+=3000;
  if(scenario=="delayed")g_test_millis+=2500;
  if(scenario=="profile")o.revokeAuthorizationAsync(2);
  CanOwnerTestAccess::service(m);
  if(scenario=="result-race")o.revokeAuthorizationAsync(2);
  o.tick(millis(),demand,true,scenario=="profile"?2:1);
  assert(o.stopped());assert(o.samples().acquired==0);assert(o.samples().source_id==0);
  const bool success=scenario=="single" || scenario=="eight";
  if(success) {
    assert(millis()-tx_at>=999);assert(!m.faultLocked());
    assert(Serial.lines.find("Learning=1,LIVE=0")!=std::string::npos);
    assert(Serial.lines.find("bitmap=12345678")!=std::string::npos);
  } else assert(Serial.lines.find("Learning=1")==std::string::npos);
  assert(m.stats().tx_success<=(scenario=="txfail"?0U:1U));
  assert(m.stats().signal_v2_fc==0);
  const auto before=m.stats().tx_attempts;
  assert(!CanOwnerTestAccess::second(m,1));
  for(unsigned i=0;i<5;++i)o.tick(millis()+i,demand,true,1);
  assert(m.stats().tx_attempts==before);
  if(success) {
    // New ARM gets a new epoch; old-generation serial RX cannot become evidence.
    g_test_millis+=2101;assert(o.armDiscovery11(millis(),3,false));
    scenario="old";tx_at=0;injected=false;
    o.tick(millis(),demand,true,3);CanOwnerTestAccess::service(m);
    const auto count=Serial.lines.size();o.tick(millis(),demand,true,3);
    assert(o.stopped());assert(Serial.lines.substr(count).find("Learning=1")==std::string::npos);
  }
  assert(CanOwnerTestAccess::fc(m,1)!=ESP_OK);
  std::cout<<"PASS P5 "<<argv[1]<<" tx="<<m.stats().tx_success<<" fc="<<m.stats().signal_v2_fc<<" LIVE="<<o.samples().acquired<<"\n";
}
