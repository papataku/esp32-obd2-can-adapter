// Regression adapted from independent reviewer-evidence/fc_bound.cpp.
#include "diagnostic_transaction.h"
#include "signal_v2_can_budget.h"
#include <cassert>
#include <cstring>
using namespace m5can;
static TransactionFrame f(const uint8_t *data, unsigned dlc, uint64_t t) {
  TransactionFrame out{};out.can_id=0x18DAF101;out.extended=true;
  out.dlc=dlc;out.timestamp_us=t;std::memcpy(out.data,data,dlc);return out;
}
int main(){
  DiagnosticRequest probe{};probe.signal_v2=true;probe.can_id=0x18DA01F1;
  probe.length=3;probe.data[0]=1;probe.data[1]=0x0c;probe.data[2]=0x0d;
  DiagnosticTransaction discovery;discovery.begin(probe,1);
  const uint8_t pair[]={6,0x41,0x0c,0x13,0x88,0x0d,0};
  discovery.onFrame(f(pair,7,2));auto evidence=discovery.result();
  assert(evidence.response_count==1); // Profile-approved physical discovery.
  DiagnosticRequest req{};req.signal_v2=true;req.can_id=0x18DA01F1;req.length=6;
  const uint8_t p[]={1,0x0d,0x0c,0x05,0x5b,0x9a};std::memcpy(req.data,p,6);
  DiagnosticTransaction tx;tx.begin(req,1000);
  const uint8_t ff[]={0x10,0x11,0x41,0x0d,0x40,0x0c,0x13,0x88};
  const uint8_t cf1[]={0x21,0x05,0x80,0x5b,0x7f,0x9a,0,0};
  const uint8_t cf2[]={0x22,0x4b,0,0,0x64};
  assert(tx.onFrame(f(ff,8,2000)).event==TransactionEvent::NeedFlowControl);
  assert(tx.onFrame(f(cf1,8,2400)).event==TransactionEvent::ResponseProgress);
  assert(tx.onFrame(f(cf2,5,2800)).event==TransactionEvent::Completed);
  unsigned fc=1;
  for(unsigned i=1;i<20;++i){
    for(auto frame : {f(ff,8,2000+i*2000), f(cf1,8,2400+i*2000),f(cf2,5,2800+i*2000)}){
      const auto event=tx.onFrame(frame).event;
      fc+=event==TransactionEvent::NeedFlowControl;
      assert(event==TransactionEvent::ProtocolError);
    }
  }
  assert(fc==1 && tx.result().status==DiagnosticStatus::IsoTpError);
  assert(tx.result().response_count==0 && tx.result().completed_at_us==0);
  assert(tx.result().raw_frame_count<=10);
  const uint8_t sf[]={3,0x41,0x0d,0};
  tx.begin(req,1000);assert(tx.onFrame(f(sf,4,2000)).event==TransactionEvent::Completed);
  assert(tx.onFrame(f(sf,4,2100)).event==TransactionEvent::ProtocolError);
  // FF group overlapping an already-completed PID must not emit even one FC.
  tx.begin(req,1000);tx.onFrame(f(sf,4,2000));
  assert(tx.onFrame(f(ff,8,2100)).event==TransactionEvent::ProtocolError);
  const uint8_t nrc[]={3,0x7f,1,0x11};
  tx.begin(req,1000);assert(tx.onFrame(f(nrc,4,2000)).event==TransactionEvent::Completed);
  assert(tx.onFrame(f(nrc,4,2100)).event==TransactionEvent::ProtocolError);
  // Response-pending storms consume RAW/RX capacity and latch invalid.
  const uint8_t pending[]={3,0x7f,1,0x78};tx.begin(req,1000);
  for(unsigned i=0;i<10;++i)tx.onFrame(f(pending,4,2000+i));
  assert(tx.onFrame(f(pending,4,2011)).event==TransactionEvent::ProtocolError);
  assert(tx.result().raw_frame_count==10);
  using signal_v2::CanBudget;
  CanBudget budget;
  assert(!budget.reserve(1,CanBudget::DiscoveryFrames,120));
  assert(budget.reserve(1,12,120));assert(budget.reserve(201,12,120));
  // Response at +120ms still contributes at the 100ms/1s boundaries.
  assert(!budget.reserve(220,12,120));
  budget.refresh(221);assert(budget.used100==2400);
  budget.refresh(1001);assert(budget.used1000==4800);
  budget.refresh(1121);assert(budget.used1000==2400);
  budget.refresh(1321);assert(budget.used100==0 && budget.used1000==0);
  assert(budget.peak100==4800 && budget.peak1000==4800);
  CanBudget actual;
  for(unsigned i=0;i<25;++i)assert(actual.observe(1000+i));
  assert(!actual.observe(1025) && actual.used100==5200 && actual.peak100==5200);
  actual.refresh(1125);assert(actual.used100==0 && actual.used1000==5200);
  assert(actual.observe(1950)); // late reply still counted in 1s window
  actual.refresh(2025);assert(actual.used1000==200);
  for(unsigned i=0;i<100;++i)actual.observe(2030);
  assert(actual.used1000==UINT32_MAX && !actual.permits(2031,1));
  actual.refresh(3030);assert(actual.used1000==0);

}
