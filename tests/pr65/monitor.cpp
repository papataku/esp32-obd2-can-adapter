#include <array>
#include <atomic>
#include <string>
#include <vector>
#include <cassert>
#include <iostream>
#include <algorithm>
#define private public
#include "can_monitor.h"
#undef private
#include "signal_v2_can_owner.h"
namespace signal_v2 {
struct CanOwnerTestAccess {
  static bool accepts(const m5can::DiagnosticResult& r) {
    CanOwner o;o.probe_target_=0x18DAF101;o.probeResult(r);
    return o.ecu_.fixed()&&!o.stopped();
  }
};
}
using namespace m5can;
struct Scheduled { uint32_t at; twai_message_t frame; };
static std::vector<Scheduled> rx;
static bool statusFails=false;
static uint32_t lossAt=UINT32_MAX;
esp_err_t twai_get_status_info(twai_status_info_t* info) {
  if(statusFails)return ESP_FAIL;
  if(g_test_millis>=lossAt)info->rx_missed_count=info->rx_overrun_count=1;
  return ESP_OK;
}
static std::vector<uint32_t> txTimes;
static std::vector<twai_message_t> sent;
esp_err_t twai_receive(twai_message_t* m,TickType_t wait) {
  if(!rx.empty() && rx.front().at<=g_test_millis+wait) {
    g_test_millis=std::max(g_test_millis,rx.front().at);
    *m=rx.front().frame;rx.erase(rx.begin());return ESP_OK;
  }
  g_test_millis+=wait;return ESP_FAIL;
}
esp_err_t twai_transmit(const twai_message_t* m,TickType_t) {
  txTimes.push_back(g_test_millis);sent.push_back(*m);return ESP_OK;
}
static twai_message_t frame(uint32_t id,std::initializer_list<uint8_t> d) {
  twai_message_t f{};f.extd=1;f.identifier=id;f.data_length_code=d.size();
  std::copy(d.begin(),d.end(),f.data);return f;
}
static DiagnosticRequest probe() {
  DiagnosticRequest q{};q.signal_v2=true;q.can_id=0x18DA01F1;q.length=3;
  q.timeout_ms=120;q.data[0]=1;q.data[1]=0x0C;q.data[2]=0x0D;return q;
}
static DiagnosticResult run(CanMonitor& m,const DiagnosticRequest& q) {
  m.acquireLease(550);DiagnosticResult r{};m.performQuery(q,r);return r;
}
int main(int argc,char** argv) {
  const std::string selected=argc>1?argv[1]:"all";
  auto test=[&](const char* id){return selected=="all"||selected==id;};
  const auto good=frame(0x18DAF101,{6,0x41,0x0C,0x13,0x88,0x0D,0});
  auto q=probe();
  if(test("controls")){CanMonitor m;g_test_millis=500;rx.clear();assert(m.signal_budget_.reserve(500,24,120));
   auto n=sent.size();auto r=run(m,q);assert(r.status==DiagnosticStatus::RateLimited&&sent.size()==n);
   std::cout<<"PASS lower reservation budget denies entire probe, zero TX\n";}
  if(test("controls")){CanMonitor m;g_test_millis=750;rx.clear();for(int i=0;i<24;++i)assert(m.recordObservedDiagnostic(750));
   auto n=sent.size();auto r=run(m,q);assert(r.status==DiagnosticStatus::RateLimited&&sent.size()==n);
   std::cout<<"PASS lower observed-traffic budget denies entire probe, zero TX\n";}
  if(test("controls")){CanMonitor m;g_test_millis=1000;rx={{1010,good}};
   auto r=run(m,q);assert(r.status==DiagnosticStatus::Ok&&r.response_count==1);
   m.signal_budget_.refresh(g_test_millis);assert(m.signal_budget_.used100==800);
   std::cout<<"PASS monitor precharges 800 bits; combined SF accepted\n";}
  auto rtr=good;rtr.rtr=1;
  auto oversized=good;oversized.data_length_code=9;
  auto standard=frame(0x7E8,{3,0x41,0x0D,0});standard.extd=0;
  if(test("M1")) for(const auto& bad:std::vector<twai_message_t>{
      rtr,oversized,standard,
      frame(0x18DAF102,{6,0x41,0x0C,0x13,0x88,0x0D,0}),
      frame(0x18DAF101,{0,0,0,0,0,0,0,0}),
      frame(0x18DAF101,{3,0x41,0x05,0}),
      frame(0x18DAF101,{0x30,0,0})}) {
    CanMonitor m;g_test_millis=2000;rx={{2005,bad},{2010,good}};
    auto r=run(m,q);
    assert(r.status==DiagnosticStatus::IsoTpError);
    assert(!signal_v2::CanOwnerTestAccess::accepts(r));
    assert(m.faultLocked());
    const auto after=sent.size();run(m,q);assert(sent.size()==after);
    std::cout<<"PASS M1 rejected unexpected RX id="<<std::hex<<bad.identifier
             <<" pci="<<unsigned(bad.data[0])<<std::dec
             <<" -> rejected, all_rx="<<m.stats().rx_frames<<", no pin\n";
  }
  if(test("M3")) for(bool speedFirst:{false,true}) {
   CanMonitor m;g_test_millis=3000;
   const auto speed=frame(0x18DAF101,{3,0x41,0x0D,0});
   const auto rpm=frame(0x18DAF101,{4,0x41,0x0C,0x13,0x88});
   rx={{3010,speedFirst?speed:rpm},{3110,speedFirst?rpm:speed}};
   auto r=run(m,q);
   assert(r.status==DiagnosticStatus::Ok&&r.response_count==2&&r.raw_frame_count==2);
   assert(signal_v2::CanOwnerTestAccess::accepts(r));
   assert(g_test_millis==3120); // No premature quiet-close after either PID.
   std::cout<<"PASS M3 both split SF orders, second at +110ms\n";
  }
  for(uint8_t pci:{uint8_t(0x10),uint8_t(0x21)}) {
    CanMonitor m;g_test_millis=4000;rx={{4010,frame(0x18DAF101,{pci,8,0x41,0x0C,0,0,0,0})}};
    auto n=sent.size();auto r=run(m,q);
    assert(r.status==DiagnosticStatus::IsoTpError&&sent.size()==n+1);
  }
  std::cout<<"PASS FF/CF fail with zero FC\n";
  if(test("controls")){CanMonitor m;g_test_millis=4500;
   rx={{4510,frame(0x18DAF101,{4,0x41,0x0C,0x13,0x88})},
       {4511,frame(0x18DAF101,{3,0x41,0x0D,0})},
       {4512,frame(0x18DAF101,{3,0x41,0x0D,0})}};
   auto n=sent.size();auto r=run(m,q);
   assert(r.status==DiagnosticStatus::IsoTpError&&sent.size()==n+1);
   std::cout<<"PASS third probe frame fails with zero FC\n";}
  if(test("controls")){CanMonitor m;g_test_millis=5000;rx={{5010,good},{5020,good}};
   auto r=run(m,q);assert(r.status==DiagnosticStatus::IsoTpError);
   std::cout<<"PASS duplicate SF fails\n";}
  if(test("controls")){CanMonitor m;g_test_millis=6000;rx={{6010,frame(0x18DAF101,{3,0x7F,1,0x11})}};
   auto r=run(m,q);assert(r.response_count==1&&r.responses[0].data[0]==0x7F);
   std::cout<<"PASS negative remains evidence for owner rejection\n";}
  if(test("M2")){CanMonitor m;g_test_millis=7000;rx={{7010,good}};auto r=run(m,q);
   assert(r.status==DiagnosticStatus::Ok);
   // This is how idle rxTask handles a reply arriving after completion.
   g_test_millis=7130;m.captureMessage(good,7130000);
   assert(m.faultLocked());
   assert(m.stats().signal_v2_rx==2&&m.stats().observed1000==600);
   rx.clear();auto n=sent.size();run(m,q);assert(sent.size()==n);
   std::cout<<"PASS M2 late SF stops and blocks next TX\n";}
  if(test("M2")){CanMonitor m;g_test_millis=7200;rx={{7210,good},{7230,good}};
    const auto n=sent.size();auto r=run(m,q);
    assert(r.status==DiagnosticStatus::IsoTpError&&m.faultLocked());
    assert(sent.size()==n+1); // A duplicate past the old 10ms quiet-close gets no FC.
    std::cout<<"PASS M2 extra response past old quiet-close latches STOP\n";
  }
  if(test("M2")){CanMonitor m;g_test_millis=7400;rx={{7410,good}};
    assert(run(m,q).status==DiagnosticStatus::Ok);
    // A queued next query must not erase a late frame on driver reinstall.
    rx={{7530,good}};g_test_millis=7600;
    const auto n=sent.size();run(m,q);
    assert(m.faultLocked()&&sent.size()==n&&m.stats().signal_v2_rx==2);
    std::cout<<"PASS M2 queued late response drained before driver reinstall\n";
  }
  if(test("M6")){CanMonitor m;g_test_millis=8000;rx.clear();auto n=sent.size();run(m,q);
   g_test_millis=8150;run(m,q);
   assert(sent.size()==n+1);
   std::cout<<"PASS M6 monitor denies probe starts 150ms apart\n";}
  if(test("m1")){CanMonitor m;g_test_millis=9000;rx.clear();q.can_id=0x18DB33F1;
   assert(!DiagnosticPolicy::allowedReadRequest(q));
   auto n=sent.size();auto r=run(m,q);assert(sent.size()==n&&r.status==DiagnosticStatus::InvalidRequest);
   std::cout<<"PASS m1 policy rejects functional; PASS monitor blocks actual TX\n";}
  if(test("M7")) for(bool unavailable:{false,true}) {
    CanMonitor m;g_test_millis=9500;rx={{9510,good}};
    if(unavailable)statusFails=true;else lossAt=9510;
    const auto n=sent.size();auto r=run(m,q);
    assert(m.faultLocked()&&!signal_v2::CanOwnerTestAccess::accepts(r));
    assert(sent.size()==n+(unavailable?0:1));
    const auto after=sent.size();run(m,q);assert(sent.size()==after);
    statusFails=false;lossAt=UINT32_MAX;
    std::cout<<"PASS M7 monitor loss/unavailable status blocks pin and TX\n";
  }
  if(test("controls")){CanMonitor m;g_test_millis=10000;rx.clear();q=probe();q.length=6;
   const uint8_t d[]={1,0x0D,0x0C,5,0x5B,0x9A};std::copy(d,d+6,q.data);
   assert(DiagnosticPolicy::allowedReadRequest(q));auto n=sent.size();run(m,q);
   m.signal_budget_.refresh(g_test_millis);
   assert(sent.size()==n+1&&m.signal_budget_.used1000==2400);
   std::cout<<"PASS regular six-byte group permitted, charged 2400 bits\n";}
  if(test("controls")){CanMonitor m;g_test_millis=11000;q=probe();q.length=6;
   const uint8_t d[]={1,0x0D,0x0C,5,0x5B,0x9A};std::copy(d,d+6,q.data);
   rx={{11010,frame(0x18DAF101,{0x10,17,0x41,0x0D,0,0x0C,0x13,0x88})},
       {11020,frame(0x18DAF101,{0x21,5,0x50,0x5B,0x64,0x9A,1,2})},
       {11030,frame(0x18DAF101,{0x22,3,4,5,6})}};
   auto n=sent.size();auto r=run(m,q);
   assert(r.status==DiagnosticStatus::Ok&&r.response_count==1&&r.responses[0].length==17);
   assert(sent.size()==n+2&&sent.back().data[0]==0x30);
   std::cout<<"PASS regular group FF+2CF, one FC, 17-byte result\n";}
  if(test("R1")){
    CanMonitor m;g_test_millis=12000;q=probe();rx={{12010,good}};
    assert(run(m,q).status==DiagnosticStatus::Ok);
    // Preserve discovery -> first regular 200ms guard even for essential.
    q.signal_v2_essential=true;g_test_millis=12150;rx.clear();
    const auto before=sent.size();
    assert(run(m,q).status==DiagnosticStatus::RateLimited && sent.size()==before);
    unsigned full=0,essential=0;
    for(uint32_t t=12200;t<=15400;t+=100){
      g_test_millis=t;q=probe();
      // Mirror owner admission at this exact boundary, retaining durations.
      m.signal_budget_.refresh(t);
      const bool group=m.signal_budget_.used100+3200<=5000 &&
                       m.signal_budget_.used1000+3200<=25000;
      if(group){
        ++full;q.length=6;
        const uint8_t d[]={1,0x0D,0x0C,5,0x5B,0x9A};std::copy(d,d+6,q.data);
        rx={{t+10,frame(0x18DAF101,{0x10,17,0x41,0x0D,0,0x0C,0x13,0x88})},
            {t+20,frame(0x18DAF101,{0x21,5,0x50,0x5B,0x64,0x9A,1,2})},
            {t+30,frame(0x18DAF101,{0x22,3,4,5,6})}};
      }else{++essential;q.signal_v2_essential=true;rx={{t+10,good}};}
      const auto n=sent.size();const auto r=run(m,q);
      assert(r.status==DiagnosticStatus::Ok && r.response_count==1 && !m.faultLocked());
      assert(g_test_millis<t+100 && sent.size()==n+(group?2:1));
      assert(m.signal_budget_.peak100<=5000 && m.signal_budget_.peak1000<=25000);
    }
    assert(full && essential && full+essential==33);
    std::cout<<"PASS R1 monitor sustained 10Hz admission, essential zero FC, M6 guard\n";
    // Even a marked essential read is indivisible at the lower boundary.
    CanMonitor blocked;g_test_millis=16000;rx.clear();q=probe();q.signal_v2_essential=true;
    assert(blocked.signal_budget_.reserve(16000,24,120));const auto n=sent.size();
    assert(run(blocked,q).status==DiagnosticStatus::RateLimited && sent.size()==n);
    // Essential keeps the SF contract: FF cannot cause FC.
    CanMonitor bad;g_test_millis=17000;
    rx={{17010,frame(0x18DAF101,{0x10,8,0x41,0x0C,0,0,0,0})}};
    const auto b=sent.size();assert(run(bad,q).status==DiagnosticStatus::IsoTpError);
    assert(sent.size()==b+1 && bad.faultLocked());
    std::cout<<"PASS R1 essential lower denial zero TX and FF zero FC\n";
  }

}
