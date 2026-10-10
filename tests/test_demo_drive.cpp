#include <cassert>
#include <cmath>
#include <initializer_list>
#include "demo_drive.h"
int main() {
  using m5can::DemoDrive;
  const auto stopped=DemoDrive::sample(2000);
  assert(stopped.rpm==0 && stopped.speed_kmh==0);
  const auto ev=DemoDrive::sample(12000);
  assert(ev.rpm==0 && ev.speed_kmh>0 && ev.power_kw>0);
  const auto accelerating=DemoDrive::sample(26000);
  assert(accelerating.rpm>1000 && accelerating.speed_kmh>40);
  const auto regen=DemoDrive::sample(49000);
  assert(regen.power_kw<0 && regen.rpm==0);
  assert(DemoDrive::sample(150000).coolant_c>DemoDrive::sample(0).coolant_c);
  for (uint32_t t=0;t<100000;t+=137) {
    const auto v=DemoDrive::sample(t);
    assert(v.rpm<=7000 && v.speed_kmh<=160 && v.coolant_c>=-40 &&
           std::isfinite(v.power_kw));
    for(uint8_t pid : {0x0c,0x0d,0x05,0x5b,0x9a}) {
      m5can::DiagnosticRawFrame frames[2]{};
      const auto n=DemoDrive::frames(pid,t,frames);
      assert(n==(pid==0x9a?2:1));
      assert(frames[0].can_id==0x18DAF101U);
      assert(frames[0].data[1]==0x41 || frames[0].data[2]==0x41);
      if(pid==0x9a) {
        const uint16_t raw=static_cast<uint16_t>(frames[1].data[1]<<8|frames[1].data[2]);
        const int16_t current10=static_cast<int16_t>(raw);
        const float kw=256.0f*(current10/10.0f)/1000.0f;
        assert(std::fabs(kw-v.power_kw)<0.015f);
      }
    }
  }
}
