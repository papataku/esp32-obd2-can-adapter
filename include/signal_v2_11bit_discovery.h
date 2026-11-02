#pragma once
#if defined(M5CAN_SIGNAL_V2_11BIT_DISCOVERY_LAB)
#if M5CAN_SIGNAL_V2_11BIT_DISCOVERY_LAB != 1 || !defined(M5CAN_SIGNAL_V2_SERIAL_HIL) || M5CAN_SIGNAL_V2_SERIAL_HIL != 1 || !defined(M5CAN_SIGNAL_V2_STAGE_GATED) || M5CAN_SIGNAL_V2_STAGE_GATED != 1 || !defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL) || M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL != 1 || defined(M5CAN_SIGNAL_V2_RP8_PROFILE) || defined(M5CAN_SIGNAL_V2_OWNER_CONTINUOUS_LIVE) || defined(M5CAN_SIGNAL_V2_OWNER_AUTO_ARM)
#error "P5 discovery requires dedicated stage-gated CAN-less lab; owner_live/RP8 forbidden"
#endif
#include <cstdint>
#include <cstring>
namespace signal_v2 {
// RAM-only Learning evidence, no binding/sample/profile/store API.
struct Discovery11 {
  static constexpr uint32_t ReceiveMs=120, ObserveMs=1000, DeadlineMs=3000;
  static constexpr unsigned TxLimit=1, RxLimit=16, ResponderLimit=8, FcLimit=0;
  static constexpr unsigned Frames=TxLimit+RxLimit, Bits=Frames*200;
  static constexpr uint8_t Request[8]={2,1,0,0,0,0,0,0};
  struct Candidate {uint32_t id=0,bitmap=0;};
  Candidate candidates[ResponderLimit]{};
  unsigned count=0,rx=0;
  uint64_t tx_us=0;
  bool guard=true,receiving=false,failed=false;
  static bool validRequest(uint32_t id,unsigned n,const uint8_t* p) {
    return id==0x7DF && n==2 && p[0]==1 && p[1]==0;
  }
  bool receive(uint32_t id,bool ext,bool rtr,bool self,unsigned dlc,const uint8_t* p,uint64_t at) {
    if(!guard)return true;
    if(++rx>RxLimit || !receiving || at<tx_us || at-tx_us>ReceiveMs*1000ULL ||
       ext || id<0x7E8 || id>0x7EF || rtr || self || dlc!=8 ||
       p[0]!=6 || p[1]!=0x41 || p[2]!=0 || count>=ResponderLimit) {
      failed=true;return false;
    }
    for(unsigned i=0;i<count;++i)if(candidates[i].id==id){failed=true;return false;}
    candidates[count].id=id;
    candidates[count].bitmap=(uint32_t(p[3])<<24)|(uint32_t(p[4])<<16)|(uint32_t(p[5])<<8)|p[6];
    ++count;
    return true;
  }
};
static_assert(Discovery11::ReceiveMs==120 && Discovery11::ObserveMs>=1000 && Discovery11::DeadlineMs==3000,"O-3 windows");
static_assert(Discovery11::TxLimit==1 && Discovery11::RxLimit==16 && Discovery11::ResponderLimit==8 && Discovery11::FcLimit==0 && Discovery11::Bits==3400,"fixed owner limits");
} // namespace signal_v2
#endif
