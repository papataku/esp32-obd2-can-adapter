#pragma once
// Aggressive CAN-DISCONNECTED bench stress demo: synthetic read-only Mode01
// responses only. Never transmit demo frames to the actual vehicle CAN bus.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "diagnostic_policy.h"

namespace m5can {
struct DemoValues {
  uint16_t rpm = 0;
  uint8_t speed_kmh = 0;
  int8_t coolant_c = 45;
  uint8_t soc_raw = 148;
  float power_kw = 0;
  float voltage_v = 256.0f;
  float current_a = 0;
};
class DemoDrive {
 public:
  static constexpr uint32_t kCycleMs = 24000U; // 24-second seamless stress cycle
  static DemoValues sample(uint32_t elapsed_ms) {
    constexpr float pi = 3.14159265358979323846f;
    const float t = static_cast<float>(elapsed_ms % kCycleMs) / 1000.0f;
    const float phase = 2.0f*pi*t/24.0f;
    const auto clampf=[](float v,float lo,float hi) {
      return std::max(lo,std::min(hi,v));
    };
    // Accelerate to ~150km/h, brake to zero, repeat twice per cycle.
    // Added harmonic motion makes short intervals reveal stale/jerky needles.
    const float speed=clampf(
      75.0f*(1.0f-std::cos(2.0f*phase))+
      9.0f*std::sin(6.0f*phase)*std::sin(phase)*std::sin(phase),
      0.0f,160.0f);
    // Engine is OFF while moving during EV segments; the ICE ramps rapidly
    // to 6000rpm and back without discontinuities when EV/ICE switches.
    float rpm=0;
    if (t>=3.0f && t<10.0f) {
      const float envelope=std::sin(pi*(t-3.0f)/7.0f);
      rpm=6100.0f*envelope*envelope;
    } else if (t>=15.0f && t<22.0f) {
      const float envelope=std::sin(pi*(t-15.0f)/7.0f);
      rpm=6200.0f*envelope*envelope;
    }
    // Positive motoring on acceleration, negative current on regeneration.
    const float power=104.0f*std::sin(2.0f*phase)+
                       15.0f*std::sin(6.0f*phase);
    // Voltage and current vary independently, but power = V * I / 1000.
    const float voltage=clampf(270.0f+
        78.0f*std::sin(phase+0.15f)+
        19.0f*std::sin(5.0f*phase+0.1f),175.0f,385.0f);
    // Deliberately aggressive changes for display stress, NOT a plausible
    // minute-by-minute real vehicle SOC or coolant trajectory.
    const float coolant=clampf(72.0f+
        29.0f*std::sin(3.0f*phase-0.35f)+
        12.0f*std::sin(6.0f*phase+0.4f),25.0f,115.0f);
    const float soc=clampf(55.0f+
        30.0f*std::cos(phase-0.3f)+
        8.0f*std::sin(5.0f*phase+0.2f),12.0f,95.0f);
    DemoValues v{};
    v.rpm=static_cast<uint16_t>(std::lround(clampf(rpm,0.0f,6500.0f)));
    v.speed_kmh=static_cast<uint8_t>(std::lround(speed));
    v.coolant_c=static_cast<int8_t>(std::lround(coolant));
    v.soc_raw=static_cast<uint8_t>(std::lround(soc*255.0f/100.0f));
    v.power_kw=power;
    v.voltage_v=voltage;
    v.current_a=1000.0f*power/voltage;
    return v;
  }

  static uint8_t frames(uint8_t pid,uint32_t elapsed_ms,DiagnosticRawFrame out[2]) {
    if (!out) return 0;
    const DemoValues v=sample(elapsed_ms);
    out[0]={}; out[1]={};
    out[0].can_id=0x18DAF101U;
    out[0].dlc=8;
    if (pid==0x0C) {
      const uint16_t raw=static_cast<uint16_t>(v.rpm*4U);
      const uint8_t data[8]={0x04,0x41,0x0C,
          static_cast<uint8_t>(raw>>8),static_cast<uint8_t>(raw),0,0,0};
      for(int i=0;i<8;++i) out[0].data[i]=data[i];
      return 1;
    }
    if (pid==0x0D || pid==0x05 || pid==0x5B) {
      const uint8_t value=pid==0x0D?v.speed_kmh:
          pid==0x05?static_cast<uint8_t>(v.coolant_c+40):v.soc_raw;
      const uint8_t data[8]={0x03,0x41,pid,value,0,0,0,0};
      for(int i=0;i<8;++i) out[0].data[i]=data[i];
      return 1;
    }
    if (pid==0x9A) {
      // Existing RP8 decoder: voltage raw/64, signed current raw/10;
      // all 3 values (V, I, kW) derive from the same sampled state.
      const uint16_t voltage64=static_cast<uint16_t>(std::lround(v.voltage_v*64.0f));
      const int16_t amps10=static_cast<int16_t>(std::lround(v.current_a*10.0f));
      const uint16_t current_bits=static_cast<uint16_t>(amps10);
      const uint8_t first[8]={0x10,0x08,0x41,0x9A,0x07,0x00,
          static_cast<uint8_t>(voltage64>>8),static_cast<uint8_t>(voltage64)};
      const uint8_t next[8]={0x21,static_cast<uint8_t>(current_bits>>8),
          static_cast<uint8_t>(current_bits),0,0,0,0,0};
      for(int i=0;i<8;++i) {out[0].data[i]=first[i];out[1].data[i]=next[i];}
      out[1].can_id=out[0].can_id;
      out[1].dlc=8;
      return 2;
    }
    return 0;
  }
};
} // namespace m5can
