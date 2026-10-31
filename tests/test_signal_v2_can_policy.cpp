#include "signal_v2_can_samples.h"
#include "signal_v2_can_budget.h"
#include "signal_v2_can_route.h"
#include <array>
#include <cassert>
using namespace signal_v2;
int main(){
  const auto stdRoute=EcuRoute::fromResponse(0x7E8U,false);
  assert(stdRoute.width==CanIdWidth::Standard11);
  assert(stdRoute.request_id==0x7E0U && !stdRoute.enabledForThisTrial());
  assert(!stdRoute.matches(EcuRoute::fromResponse(0x18DAF101U,true)));
  const auto extRoute=EcuRoute::fromResponse(0x18DAF106U,true);
  assert(extRoute.width==CanIdWidth::Extended29);
  assert(extRoute.request_id==0x18DA06F1U &&
         extRoute.enabledForThisTrial());
  assert(!EcuRoute::fromResponse(0x7E8U,true).enabledForThisTrial());

  CanBudget one;
  for(unsigned i=0;i<10;++i)assert(one.reserve(i*100,12));
  assert(!one.reserve(999,12)); // rolling 5% at 500kbps
  assert(one.denied==1&&one.peak1000==24000);
  assert(one.reserve(1000,12));
  CanBudget shortWindow;
  assert(shortWindow.reserve(1,12));
  assert(shortWindow.reserve(2,12));
  assert(!shortWindow.reserve(3,12)); // rolling 10% at 100ms
  const uint8_t full[]={0x41,0x0D,0x40,0x0C,0x13,0x88,0x05,0x80,
                        0x5B,0x7F,0x9A,0,0,0x4B,0,0,0x64};
  std::array<CanSamples::Sample,5> out{};
  assert(CanSamples::decodeGroup(full,sizeof(full),true,out));
  assert(out[0].raw[2]==0x40&&out[1].raw[2]==0x13&&out[4].length==8);
  assert(!CanSamples::decodeGroup(full,sizeof(full)-1,true,out));
  auto corrupted=std::array<uint8_t,sizeof(full)>{};
  for(unsigned i=0;i<sizeof(full);++i)corrupted[i]=full[i];
  corrupted[8]=0x0C;
  assert(!CanSamples::decodeGroup(corrupted.data(),corrupted.size(),true,out));
  const uint8_t essential[]={0x41,0x0C,0x13,0x88,0x0D,0x40};
  assert(CanSamples::decodeGroup(essential,sizeof(essential),false,out));
  CanSamples samples;
  samples.values[0].length=3;samples.values[0].quality=0;
  samples.invalidate(3);
  assert(samples.sample(0x0D)->quality==3&&samples.sample(0x0D)->length==0);
}
