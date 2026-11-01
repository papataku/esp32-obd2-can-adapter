#include "signal_v2_discovery_profile.h"
#include <cassert>
#include <iostream>
int main(){
  using namespace signal_v2;
  static_assert(profiles::kLegacyRp8.deadline_ms==120 &&
    profiles::kLegacyRp8.probe_period_ms==200 && profiles::kLegacyRp8.period_5hz_ms==200 &&
    profiles::kLegacyRp8.period_10hz_ms==100 && profiles::kLegacyRp8.probe_frames==4 &&
    profiles::kLegacyRp8.group_frames==12 && profiles::kLegacyRp8.essential_frames==4 &&
    profiles::kLegacyRp8.headroom_frames==16 && profiles::kLegacyRp8.bits_per_frame==200 &&
    profiles::kLegacyRp8.bits_100ms==5000 && profiles::kLegacyRp8.bits_1s==25000,
    "P2 cannot change timing or budget values");
  const auto* p=profiles::builtin("legacy-rp8");
  assert(p && p->schema==1 && p->revision==1);
  assert(p->width==CanIdWidth::Extended29 && p->responder==0x18DAF101U);
  assert(p->request_id==0x18DA01F1U);
  const uint16_t expected[]={0x0D,0x0C,0x05,0x5B,0x9A};
  for(unsigned i=0;i<5;++i){assert(p->pids[i]==expected[i]);assert(p->group_request[i+1]==expected[i]);}
  assert(p->probe_request[0]==1 && p->probe_request[1]==0x0C && p->probe_request[2]==0x0D);
  assert(profiles::candidateCount("legacy-rp8")==1);
  for(const char* id:{static_cast<const char*>(nullptr),"","unknown"}){
    assert(!profiles::builtin(id));assert(profiles::candidateCount(id)==0);
  }
#if defined(M5CAN_SIGNAL_V2_RP8_PROFILE)
  static_assert(DiscoveryProfile::kCount==1,"explicit built-in only");
  const auto r=DiscoveryProfile::kCandidates[0].route();
  assert(r.request_id==p->request_id && r.response_id==p->responder && r.width==p->width);
#else
  static_assert(DiscoveryProfile::kCount==0,"empty trial cannot transmit");
#endif
  std::cout<<"PASS schema/revision 1, P1 PID/width/responder, unknown/empty 0 candidates\n";
}
