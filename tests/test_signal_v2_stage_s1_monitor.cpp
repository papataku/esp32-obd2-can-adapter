// PR-C2 review M-01/M-02: the S1 window must start at the driver-accepted
// submission time carried through the real CanMonitor::performQuery().
#include <algorithm>
#include <cassert>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>
#include "can_monitor.h"
#include "signal_v2_can_owner.h"
namespace signal_v2 {
struct CanOwnerTestAccess {
  static void complete(CanOwner &o,const m5can::DiagnosticResult &r){o.in_flight_=false;o.result(r);}
  static void perform(m5can::CanMonitor &m,const m5can::DiagnosticRequest &q,m5can::DiagnosticResult &r){
    m.acquireLease(550);m.performQuery(q,r);
  }
  static void capture(m5can::CanMonitor &m,const twai_message_t &frame){
    m.captureMessage(frame,static_cast<uint64_t>(g_test_millis)*1000ULL);
  }
};
}
using namespace m5can;
// Deterministic host-only scheduling point: xQueueSend(frame_queue_) occurs
// after RX counters/drop accounting but before the per-ID audit entry.
static signal_v2::CanOwner *preempt_owner=nullptr;
int s1TestFrameQueueSend(QueueHandle_t,const void*,TickType_t){
  if(preempt_owner){
    signal_v2::CanOwner *const o=preempt_owner;
    preempt_owner=nullptr;
    const signal_v2::Simulator::Demand none{};
    o->tick(1200,none,true,1); // The owner ends S1 while RX is suspended.
  }
  return pdTRUE;
}
struct Scheduled{uint32_t at;twai_message_t frame;};
static std::vector<Scheduled> rx;
static std::vector<uint32_t> tx_ms;
esp_err_t twai_get_status_info(twai_status_info_t*){return ESP_OK;}
esp_err_t twai_receive(twai_message_t *m,TickType_t wait){
  if(!rx.empty()&&rx.front().at<=g_test_millis+wait){
    g_test_millis=std::max(g_test_millis,rx.front().at);
    *m=rx.front().frame;rx.erase(rx.begin());return ESP_OK;
  }
  g_test_millis+=wait;return ESP_FAIL;
}
esp_err_t twai_transmit(const twai_message_t*,TickType_t){tx_ms.push_back(g_test_millis);return ESP_OK;}
static twai_message_t frame(uint32_t id,std::initializer_list<uint8_t> d){
  twai_message_t f{};f.extd=1;f.identifier=id;f.data_length_code=uint8_t(d.size());
  std::copy(d.begin(),d.end(),f.data);return f;
}
static DiagnosticRequest probe(){
  DiagnosticRequest q{};q.signal_v2=true;q.can_id=0x18DA01F1;q.length=3;
  q.timeout_ms=120;q.data[0]=1;q.data[1]=0x0C;q.data[2]=0x0D;return q;
}
static std::string record(){
  const auto &l=SerialStub::lines;const auto at=l.rfind("#SIGNALV2_S1,");
  return at==std::string::npos?std::string():l.substr(at,l.find('\n',at)-at);
}
int main(){
  SerialStub::capture=true;
  const signal_v2::Simulator::Demand none{};
  for(bool reply:{true,false}){
    CanMonitor m;assert(m.begin());signal_v2::CanOwner o;o.attach(&m);
    SerialStub::lines.clear();tx_ms.clear();rx.clear();
    g_test_millis=0;o.requestStageS1(1);o.tick(0,none,true,1);assert(o.busy());
    // The CAN owner task actually submits 200 ms after the enqueue.
    g_test_millis=200;
    if(reply)rx={{210,frame(0x18DAF101,{6,0x41,0x0C,0x13,0x88,0x0D,0})}};
    DiagnosticResult r{};signal_v2::CanOwnerTestAccess::perform(m,probe(),r);
    assert(tx_ms.size()==1&&tx_ms[0]==200);
    assert(r.request_tx_us==200000ULL); // survives the transaction result copy
    signal_v2::CanOwnerTestAccess::complete(o,r);
    o.tick(1000,none,true,1);assert(!o.stopped()); // not 1 s after the enqueue
    o.tick(1199,none,true,1);assert(!o.stopped());
    o.tick(1200,none,true,1);assert(o.stopped());
    const auto line=record();
    assert(line.find("request_ms=200")!=std::string::npos);
    assert(line.find(reply?"outcome=SINGLE_FRAME_CONTRACT":"outcome=NO_RESPONSE")!=std::string::npos);
    assert(line.find("stop=S1_OBSERVATION_COMPLETE")!=std::string::npos);
  }
  // M-07: a CAN frame has begun, but its audit note has not yet run.
  // A host callback emulates preemption at frame_queue_ delivery, and
  // causes the Arduino owner to close the audit before RX resumes.
  // Old implementation: complete=1 even though this frame is not audited.
  // Fixed implementation: complete=0 with rx_inflight_end=1.
  {
    CanMonitor m;assert(m.begin());signal_v2::CanOwner o;o.attach(&m);
    SerialStub::lines.clear();tx_ms.clear();rx.clear();
    g_test_millis=0;o.requestStageS1(1);o.tick(0,none,true,1);assert(o.busy());
    g_test_millis=200;
    DiagnosticResult r{};signal_v2::CanOwnerTestAccess::perform(m,probe(),r);
    assert(r.request_tx_us==200000ULL);
    signal_v2::CanOwnerTestAccess::complete(o,r);
    g_test_millis=1200;
    preempt_owner=&o;
    signal_v2::CanOwnerTestAccess::capture(
        m,frame(0x0CF00400U,{0xFF,0,0,0,0,0,0,0}));
    assert(preempt_owner==nullptr && o.stopped());
    const std::string line=record();
    assert(line.find("complete=0")!=std::string::npos);
    assert(line.find("rx_inflight_end=1")!=std::string::npos);
    assert(line.find("rx_counted=1")!=std::string::npos);
  }
  std::cout<<"PR-C2 S1 window from real driver submission PASS\n";
}
