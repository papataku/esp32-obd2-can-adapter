// PR-C2 review M-01/M-02: the S1 window must start at the driver-accepted
// submission time carried through the real CanMonitor::performQuery().
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
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
  static esp_err_t transmit(m5can::CanMonitor &m,const twai_message_t &f){
    return m.transmitOwnedFrame(f,20);
  }
  static uint64_t txAttempts(const m5can::CanMonitor &m){return m.stats_.tx_attempts;}
  static bool closePending(const m5can::CanMonitor &m){return m.audit_close_requested_.load();}
  static void serviceClose(m5can::CanMonitor &m){m.processAuditCloseOnOwner();}
};
}
using namespace m5can;
// Deterministic host-only scheduling point: xQueueSend(frame_queue_) occurs
// after RX counters/drop accounting but before the per-ID audit entry.
static signal_v2::CanOwner *preempt_owner=nullptr;
static CanMonitor *audit_owner=nullptr;
static bool owner_paused=false;
static signal_v2::CanOwner* handoff_owner=nullptr;
static bool preempt_after_receive=false;
static std::mutex driver_mu;
static std::condition_variable driver_cv;
static bool driver_block=false,driver_entered=false,driver_release=false;
void s1TestOnAuditDelay(uint32_t){
  if (audit_owner && !owner_paused &&
      signal_v2::CanOwnerTestAccess::closePending(*audit_owner))
    signal_v2::CanOwnerTestAccess::serviceClose(*audit_owner);
}
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
static CanMonitor *stop_during_tx=nullptr;
static bool stop_fired=false;
// Inject asynchronous Arduino STOP specifically after the transmitter has
// incremented tx_attempts but before its driver submission.
void s1TestOnCriticalExit(portMUX_TYPE*){
  if(stop_during_tx){
    CanMonitor* m=stop_during_tx;stop_during_tx=nullptr;
    stop_fired=true;
    m->revokeStageAuthorization(2); // Async GATT STOP revokes its epoch
  }
}
void s1TestOnDriverUninstall(){
  // Host-only 7ms TWAI reinstall blind interval, after physical TX.
  if(!tx_ms.empty())g_test_millis+=7;
}
esp_err_t twai_get_status_info(twai_status_info_t* info){
  if(info)info->msgs_to_rx=static_cast<uint32_t>(rx.size());
  return ESP_OK;
}
esp_err_t twai_receive(twai_message_t *m,TickType_t wait){
  if(!rx.empty()&&rx.front().at<=g_test_millis+wait){
    g_test_millis=std::max(g_test_millis,rx.front().at);
    *m=rx.front().frame;rx.erase(rx.begin());
    if(preempt_after_receive && handoff_owner){
      preempt_after_receive=false;owner_paused=true;
      const signal_v2::Simulator::Demand none{};
      handoff_owner->tick(1200,none,true,1);
      owner_paused=false;
    }
    return ESP_OK;
  }
  g_test_millis+=wait;return ESP_FAIL;
}
esp_err_t twai_transmit(const twai_message_t*,TickType_t){
  {
    std::unique_lock<std::mutex> lock(driver_mu);
    if(driver_block){
      driver_entered=true;driver_cv.notify_all();
      driver_cv.wait(lock,[] {return driver_release;});
    }
  }
  tx_ms.push_back(g_test_millis);
  return ESP_OK;
}
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
    audit_owner=&m;
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
    // M-06: a real non-observable driver reinstall interval is disclosed
    // even when the count-based observation record is complete.
    assert(line.find("blind_count=1")!=std::string::npos);
    assert(line.find("blind_total_us=7000")!=std::string::npos);
    assert(line.find("blind_open=0")!=std::string::npos);
    assert(line.find("complete=1")!=std::string::npos);
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
    audit_owner=&m;owner_paused=true;
    preempt_owner=&o;
    signal_v2::CanOwnerTestAccess::capture(
        m,frame(0x0CF00400U,{0xFF,0,0,0,0,0,0,0}));
    assert(preempt_owner==nullptr && o.stopped());
    const std::string line=record();
    assert(line.find("complete=0")!=std::string::npos);
    assert(line.find("rx_inflight_end=1")!=std::string::npos);
    assert(line.find("rx_counted=1")!=std::string::npos);
    assert(line.find("owner_confirmed=0")!=std::string::npos);
    owner_paused=false;
  }
  // B-03: STOP races in after the earlier lease check but before physical
  // twai_transmit. The post-STOP driver queue must remain empty.
  {
    CanMonitor m;assert(m.begin());g_test_millis=100;
    tx_ms.clear();assert(m.beginStageS1(2));assert(m.acquireLease(550));
    stop_fired=false;stop_during_tx=&m;
    const esp_err_t status=signal_v2::CanOwnerTestAccess::transmit(
        m,frame(0x18DA01F1U,{3,1,0x0C,0x0D}));
    stop_during_tx=nullptr;
    assert(stop_fired);
    assert(status!=ESP_OK);
    assert(tx_ms.empty());
    assert(signal_v2::CanOwnerTestAccess::txAttempts(m)==0);
    assert(!m.acquireLease(550));
    assert(!m.beginStageS1(2));
    assert(m.beginStageS1(3));
  }

  // M-10 (RED): the staged lower gate rejects a group frame and a second
  // otherwise-valid physical S1 frame without relying on the CAN owner.
  {
    CanMonitor monitor;assert(monitor.begin());g_test_millis=100;
    tx_ms.clear();assert(monitor.beginStageS1(7));assert(monitor.acquireLease(550));
    twai_message_t group=frame(0x18DA01F1U,{5,1,0x0D,0x0C,5,0x5B,0,0});
    assert(signal_v2::CanOwnerTestAccess::transmit(monitor,group)!=ESP_OK);
    assert(tx_ms.empty());
    const twai_message_t s1=frame(0x18DA01F1U,{3,1,0x0C,0x0D,0,0,0,0});
    assert(signal_v2::CanOwnerTestAccess::transmit(monitor,s1)==ESP_OK);
    assert(signal_v2::CanOwnerTestAccess::transmit(monitor,s1)!=ESP_OK);
    assert(tx_ms.size()==1);
  }

  // M-09: [tx_us, tx_us+1_000_000us), including exact endpoints.
  {
    ObservationAudit audit{};audit.active=true;
    audit.window_set=true;audit.window_start_us=200000;
    const uint8_t data[8]={0x06,0x41,0x0C,0,0,0,0,0};
    audit.note(0x0CF00400U,true,false,8,data,199999);  // before
    audit.note(0x0CF00400U,true,false,8,data,200000);  // inclusive
    audit.note(0x0CF00400U,true,false,8,data,1199999); // inclusive
    audit.note(0x0CF00400U,true,false,8,data,1200000); // exclusive
    assert(audit.accounted_rx==4 && audit.all_rx==2);
    assert(audit.outside_before==1 && audit.outside_after==1);
  }
  // M-08 A: driver RX returned successfully, CAN owner paused before
  // captureMessage. The Arduino STOP cannot certify the snapshot.
  {
    CanMonitor monitor;assert(monitor.begin());signal_v2::CanOwner owner;
    owner.attach(&monitor);g_test_millis=0;
    rx.clear();tx_ms.clear();SerialStub::lines.clear();
    owner.requestStageS1(1);owner.tick(0,none,true,1);
    g_test_millis=200;audit_owner=&monitor;
    DiagnosticResult result{};signal_v2::CanOwnerTestAccess::perform(monitor,probe(),result);
    signal_v2::CanOwnerTestAccess::complete(owner,result);
    g_test_millis=1200;
    rx={{1200,frame(0x0CF00400U,{0xFF,0,0,0,0,0,0,0})}};
    handoff_owner=&owner;preempt_after_receive=true;
    twai_message_t returned{};
    assert(twai_receive(&returned,0)==ESP_OK);
    handoff_owner=nullptr;
    signal_v2::CanOwnerTestAccess::capture(monitor,returned);
    assert(owner.stopped());
    assert(record().find("owner_confirmed=0")!=std::string::npos);
    assert(record().find("complete=0")!=std::string::npos);
  }
  // M-08 C: pending TWAI RX must be drained by the CAN-owner close barrier,
  // but any pending frame makes absolute completeness unverifiable.
  {
    CanMonitor monitor;assert(monitor.begin());signal_v2::CanOwner owner;
    owner.attach(&monitor);g_test_millis=0;
    rx.clear();tx_ms.clear();SerialStub::lines.clear();
    owner.requestStageS1(1);owner.tick(0,none,true,1);
    g_test_millis=200;audit_owner=&monitor;
    DiagnosticResult result{};signal_v2::CanOwnerTestAccess::perform(monitor,probe(),result);
    signal_v2::CanOwnerTestAccess::complete(owner,result);
    rx={{1200,frame(0x0CF00400U,{0xFF,0,0,0,0,0,0,0})}};
    g_test_millis=1200;owner.tick(1200,none,true,1);
    assert(owner.stopped() && rx.empty());
    assert(record().find("rx_pending_end=1")!=std::string::npos);
    assert(record().find("complete=0")!=std::string::npos);
  }
  // N-03: true two-task mutex scheduling. STOP-first means zero TX;
  // TX-first and STOP while driver is blocked means STOP does not complete
  // until the in-flight driver call returns.
  {
    CanMonitor monitor;assert(monitor.begin());g_test_millis=100;
    tx_ms.clear();assert(monitor.beginStageS1(20));
    assert(monitor.acquireLease(550));
    monitor.revokeStageAuthorization(20);
    const auto valid=frame(0x18DA01F1U,{3,1,0x0C,0x0D,0,0,0,0});
    assert(signal_v2::CanOwnerTestAccess::transmit(monitor,valid)!=ESP_OK);
    assert(tx_ms.empty());
    assert(monitor.beginStageS1(21));
    assert(monitor.acquireLease(550));
    {
      std::lock_guard<std::mutex> lock(driver_mu);
      driver_block=true;driver_entered=false;driver_release=false;
    }
    std::atomic<bool> stop_done{false};
    esp_err_t tx_result=ESP_FAIL;
    std::thread tx_task([&]{
      tx_result=signal_v2::CanOwnerTestAccess::transmit(monitor,valid);
    });
    {
      std::unique_lock<std::mutex> lock(driver_mu);
      driver_cv.wait(lock,[] {return driver_entered;});
    }
    std::thread stop_task([&]{
      monitor.revokeStageAuthorization(21);
      stop_done.store(true);
    });
    // STOP cannot complete while TWAI is still in its submission call.
    for(unsigned i=0;i<2000 && !stop_done.load();++i)std::this_thread::yield();
    assert(!stop_done.load());
    {
      std::lock_guard<std::mutex> lock(driver_mu);
      driver_release=true;
    }
    driver_cv.notify_all();
    tx_task.join();stop_task.join();
    driver_block=false;
    assert(tx_result==ESP_OK && stop_done.load());
    assert(tx_ms.size()==1);
    assert(signal_v2::CanOwnerTestAccess::transmit(monitor,valid)!=ESP_OK);
  }
  std::cout<<"PR-C2 S1 TX/STOP race + monitor integration PASS\n";
}
