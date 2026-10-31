#pragma once
#include <array>
#include <cstdint>
namespace signal_v2 {
// HOST DESIGN ONLY. V2-2 never calls this from a physical CAN TX path.
// Slots always carry speed and RPM; additional PIDs rotate round-robin.
struct GroupPlan {
  struct Slot {std::array<uint16_t,6> pids{};uint8_t count=0;};
  std::array<Slot,4> slots{};
  uint8_t count=0;
};
inline GroupPlan planSplit(const uint16_t* pids,uint8_t total){
  GroupPlan p{};
  if(!pids||total<2||total>16||pids[0]!=0x0D||pids[1]!=0x0C)return p;
  unsigned extra=total-2;
  unsigned slots=extra?(extra+3)/4:1;
  if(slots>4)return p;
  p.count=static_cast<uint8_t>(slots);
  for(unsigned i=0;i<slots;++i){
    auto &s=p.slots[i];
    s.pids[0]=0x0D;s.pids[1]=0x0C;s.count=2;
    for(unsigned j=0;j<4&&2+i*4+j<total;++j)
      s.pids[s.count++]=pids[2+i*4+j];
  }
  return p;
}
} // namespace signal_v2
