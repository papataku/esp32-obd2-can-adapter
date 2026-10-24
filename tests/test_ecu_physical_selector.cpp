#include <cassert>
#include <cstdint>
#include <iostream>
#include "ecu_physical_selector.h"
#include "diagnostic_request_pacer.h"
#include "diagnostic_transaction.h"

using namespace m5can;
static DiagnosticRequest request(uint32_t id,uint8_t length=3,uint8_t pid=0x0C){
  DiagnosticRequest q{};q.can_id=id;q.length=length;q.data[0]=1;
  q.data[1]=pid;if(length==3)q.data[2]=0x0D;return q;
}
static DiagnosticResult responses(uint8_t n=5,uint16_t rpm=0x1738) {
  const uint32_t sources[]={0x18DAF1EFU,0x18DAF10EU,0x18DAF101U,0x18DAF102U,0x18DAF106U};
  DiagnosticResult r{};r.status=DiagnosticStatus::Ok;r.response_count=n;r.raw_frame_count=n;
  for(uint8_t i=0;i<n;++i){
    auto& m=r.responses[i];m.can_id=sources[i];m.length=6;
    m.data[0]=0x41;m.data[1]=0x0C;
    const uint16_t value=static_cast<uint16_t>(rpm+(i==1?16:0));
    m.data[2]=static_cast<uint8_t>(value>>8);m.data[3]=static_cast<uint8_t>(value);
    m.data[4]=0x0D;m.data[5]=1;
    auto& f=r.raw_frames[i];f.can_id=m.can_id;f.dlc=8;f.data[0]=6;
    for(uint8_t j=0;j<6;++j)f.data[j+1]=m.data[j];
    r.raw_rx_offsets_us[i]=500+i*200;
  }
  return r;
}
int main(){
  const auto functional=request(0x18DB33F1U),physical=request(0x18DA0EF1U);
  assert(!DiagnosticPolicy::allowedReadRequest(physical));
  assert(!DiagnosticPolicy::observedPhysicalEcu(physical.can_id));
  auto r=responses();
  // Merely knowing the ID does not authorize physical TX. Raw evidence must match.
  auto tampered=r;tampered.raw_frames[1].data[3]^=0x01;
  DiagnosticPolicy::observeFunctionalPairs(functional,tampered);
  assert(!DiagnosticPolicy::observedPhysicalEcu(physical.can_id));
  DiagnosticPolicy::observeFunctionalPairs(functional,r);
  assert(DiagnosticPolicy::observedPhysicalEcu(physical.can_id));
  assert(DiagnosticPolicy::allowedReadRequest(physical));
  assert(DiagnosticPolicy::allowedReadRequest(request(physical.can_id,2,0x0D)));
  assert(!DiagnosticPolicy::allowedReadRequest(request(0x18DA03F1U)));
  assert(!DiagnosticPolicy::allowedReadRequest(request(physical.can_id,2,0x05)));
  auto mode09=request(physical.can_id,2);mode09.data[0]=0x09;
  assert(!DiagnosticPolicy::allowedReadRequest(mode09));
  auto uds=request(physical.can_id);uds.data[0]=0x22;
  assert(!DiagnosticPolicy::allowedReadRequest(uds));
  assert(DiagnosticPolicy::responseBelongsToRequest(physical,0x18DAF10EU));
  assert(!DiagnosticPolicy::responseBelongsToRequest(physical,0x18DAF101U));
  // Functional and physical requests cannot bypass the 50ms per-PID limit.
  DiagnosticRequestPacer pacer;
  assert(pacer.remainingUs(functional,1000000)==0);
  pacer.noteSuccessfulTx(functional,1000000);
  assert(pacer.remainingUs(physical,1020000)==30000);
  assert(pacer.remainingUs(request(physical.can_id,2,0x0D),1020000)==30000);
  assert(pacer.remainingUs(request(physical.can_id,2,0x05),1020000)==0);

  // Real 4rpm disagreement is legal candidate evidence, NOT a requirement
  // that all five ECU payload bytes agree.
  EcuPhysicalSelector selector;
  DiagnosticRawFrame frames[2]{};
  for(int i=0;i<8;++i) {
    auto probe=responses();
    // ECU0E changes each cycle. Other four stay steady. Less latency is
    // lower priority than the number of observed transitions.
    probe.responses[1].data[2]=0x17;
    probe.responses[1].data[3]=static_cast<uint8_t>(0x48+i*4);
    probe.raw_frames[1].data[3]=probe.responses[1].data[2];
    probe.raw_frames[1].data[4]=probe.responses[1].data[3];
    assert(selector.observe(probe,false,frames));
  }
  assert(selector.fixed() && selector.probes()==8);
  assert(selector.chosenResponse()==0x18DAF10EU);
  assert(selector.physicalId()==0x18DA0EF1U);
  const auto best=selector.candidate(1);
  assert(best.successes==8 && best.changes==7);
  auto response=responses();
  // A physical request returns ONLY the selected ECU.
  response.responses[0]=response.responses[1];
  response.raw_frames[0]=response.raw_frames[1];
  response.raw_rx_offsets_us[0]=response.raw_rx_offsets_us[1];
  response.response_count=response.raw_frame_count=1;
  assert(selector.observe(response,true,frames));
  assert(selector.physicalOk()==1);
  auto missing=response;missing.status=DiagnosticStatus::NoData;
  missing.response_count=missing.raw_frame_count=0;
  assert(!selector.observe(missing,true,frames));
  assert(!selector.observe(missing,true,frames));
  assert(!selector.observe(missing,true,frames));
  assert(selector.state()==EcuPhysicalSelector::State::Recheck);
  assert(selector.retries()==1);
  for(int i=0;i<8;++i) assert(selector.observe(responses(),false,frames));
  // In this recheck the minimum-ID ECU01 has fewer changes than ECU0E,
  // and becomes highest ranked. Replacing the pinned ECU is prohibited.
  assert(selector.legacy());
  assert(selector.chosenResponse()==0x18DAF10EU);

  // Missing response reduces success count; invalid/duplicate pair disqualifies.
  EcuPhysicalSelector other;
  for(int i=0;i<8;++i){
    auto p=responses(2);
    if(i==2) {p.response_count=p.raw_frame_count=1;}
    assert(other.observe(p,false,frames));
  }
  assert(other.fixed() && other.chosenResponse()==0x18DAF1EFU);
  EcuPhysicalSelector bad;
  for(int i=0;i<8;++i){
    auto p=responses(1);
    p.responses[0].length=4;
    assert(!bad.observe(p,false,frames));
  }
  assert(bad.legacy());
  std::cout<<"PASS Issue54 observed-ECU allowlist, physical source isolation, 50ms and ranking\n";
}
