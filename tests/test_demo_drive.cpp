#include <cassert>
#include <cmath>
#include <initializer_list>
#include <algorithm>
#include "demo_drive.h"

int main() {
  using m5can::DemoDrive;
  assert(DemoDrive::sample(0).speed_kmh==0);
  const auto ev=DemoDrive::sample(1000);
  assert(ev.rpm==0 && ev.speed_kmh>0 && ev.power_kw>0);
  const auto ice=DemoDrive::sample(6500);
  assert(ice.rpm>3000 && ice.speed_kmh>0);
  const auto regen=DemoDrive::sample(9000);
  assert(regen.power_kw<0);
  // Seamless repeat and continuous data (not a giant frame-discontinuity).
  assert(DemoDrive::sample(DemoDrive::kCycleMs).rpm==DemoDrive::sample(0).rpm);
  assert(DemoDrive::sample(DemoDrive::kCycleMs).speed_kmh==
         DemoDrive::sample(0).speed_kmh);
  float lo_c=200,hi_c=-100,lo_soc=100,hi_soc=0;
  float lo_v=1000,hi_v=0,lo_i=1000,hi_i=-1000,lo_p=200,hi_p=-200;
  unsigned ev_moving=0,regen_samples=0;
  for(uint32_t t=0;t<DemoDrive::kCycleMs;t+=37) {
    const auto v=DemoDrive::sample(t);
    assert(v.rpm<=6500 && v.speed_kmh<=160);
    assert(v.coolant_c>=25 && v.coolant_c<=115);
    assert(v.voltage_v>=175 && v.voltage_v<=385);
    assert(v.soc_raw<=255 && std::isfinite(v.power_kw));
    assert(std::fabs(v.current_a*v.voltage_v/1000.0f-v.power_kw)<0.0001f);
    if(v.rpm==0 && v.speed_kmh>5) ++ev_moving;
    if(v.power_kw< -20) ++regen_samples;
    const float soc=v.soc_raw*100.0f/255.0f;
    lo_c=std::min(lo_c,static_cast<float>(v.coolant_c));
    hi_c=std::max(hi_c,static_cast<float>(v.coolant_c));
    lo_soc=std::min(lo_soc,soc); hi_soc=std::max(hi_soc,soc);
    lo_v=std::min(lo_v,v.voltage_v); hi_v=std::max(hi_v,v.voltage_v);
    lo_i=std::min(lo_i,v.current_a); hi_i=std::max(hi_i,v.current_a);
    lo_p=std::min(lo_p,v.power_kw); hi_p=std::max(hi_p,v.power_kw);
    for(uint8_t pid : {0x0c,0x0d,0x05,0x5b,0x9a}) {
      m5can::DiagnosticRawFrame frames[2]{};
      const auto n=DemoDrive::frames(pid,t,frames);
      assert(n==(pid==0x9a?2:1));
      assert(frames[0].can_id==0x18DAF101U);
      assert(frames[0].data[1]==0x41 || frames[0].data[2]==0x41);
      if(pid==0x9a) {
        const uint16_t volt_raw=static_cast<uint16_t>(
            frames[0].data[6]<<8|frames[0].data[7]);
        const float voltage=volt_raw/64.0f;
        const uint16_t i_raw=static_cast<uint16_t>(
            frames[1].data[1]<<8|frames[1].data[2]);
        const int16_t amps10=static_cast<int16_t>(i_raw);
        const float current=amps10/10.0f;
        assert(std::fabs(voltage-v.voltage_v)<0.016f);
        assert(std::fabs(current-v.current_a)<0.051f);
        assert(std::fabs(voltage*current/1000.0f-v.power_kw)<0.035f);
      }
    }
  }
  assert(hi_c-lo_c>60.0f);
  assert(hi_soc-lo_soc>50.0f);
  assert(hi_v-lo_v>130.0f);
  assert(hi_i-lo_i>400.0f);
  assert(hi_p-lo_p>180.0f);
  assert(ev_moving>10 && regen_samples>10);
}
