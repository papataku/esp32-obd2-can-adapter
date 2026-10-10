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
  // Bench-only 24s closed lap. Speed rises under throttle, RPM repeatedly
  // dips on upshifts while speed CONTINUES rising; hard braking induces regen.
  // Interpolate independent speed/RPM keyframes with continuous derivatives,
  // rather than an identical sinusoid / triangle for every value.
  static DemoValues sample(uint32_t elapsed_ms) {
    constexpr float pi=3.14159265358979323846f;
    struct LapPoint { uint16_t ms; float speed; float rpm; };
    static constexpr LapPoint lap[]={
      {    0,   0,    0}, { 1000,  30,    0}, // EV launch
      { 1800,  52, 3800}, { 3000,  78, 6100},
      { 3370,  86, 4150}, // 1->2: brief RPM drop, speed still rising
      { 4800, 115, 6200}, { 5150, 118, 4500}, // 2->3
      { 6800, 154, 6200}, { 7400, 148, 5600},
      { 8800, 105, 3900}, { 9550,  72, 2800}, // corner braking
      {10400, 100, 6000}, {12000, 133, 6100},
      {12400, 138, 4300}, // next upshift
      {13600, 151, 5550}, {14800,  95, 3600},
      {15600,  64, 3000}, {16800,  97, 5900},
      {18000, 123, 6200}, {18400, 130, 4450}, // next upshift
      {20100, 155, 6100}, {20800, 136, 4900},
      {22200,  79, 3200}, {23000,  34, 1000},
      {24000,   0,    0} // smooth closed-loop stop/launch
    };
    const uint32_t ms=elapsed_ms%kCycleMs;
    float speed=0,rpm=0,speed_slope=0;
    for(size_t i=1;i<sizeof(lap)/sizeof(lap[0]);++i) {
      if(ms>lap[i].ms) continue;
      const LapPoint &p=lap[i-1],&n=lap[i];
      const float duration=(n.ms-p.ms)/1000.0f;
      const float f=(ms-p.ms)/1000.0f/duration;
      const float s=f*f*(3.0f-2.0f*f);
      const float ds=6.0f*f*(1.0f-f)/duration;
      speed=p.speed+(n.speed-p.speed)*s;
      rpm=p.rpm+(n.rpm-p.rpm)*s;
      speed_slope=(n.speed-p.speed)*ds; // km/h per second
      break;
    }
    const float phase=2.0f*pi*static_cast<float>(ms)/kCycleMs;
    const auto clampf=[](float value,float minimum,float maximum) {
      return std::max(minimum,std::min(value,maximum));
    };
    // Subtle engine vibration while running (zero at loop boundaries).
    rpm+=65.0f*clampf(rpm/1200.0f,0.0f,1.0f)*std::sin(10.0f*phase);
    // Acceleration draws current, hard braking makes the power negative.
    const float power=clampf(4.8f*speed_slope+8.0f*std::sin(6.0f*phase),
                             -108.0f,108.0f);
    const float voltage=clampf(270.0f+
        78.0f*std::sin(phase+0.15f)+
        19.0f*std::sin(5.0f*phase+0.1f),175.0f,385.0f);
    // Display stress, NOT physically plausible coolant/SOC ramp rates.
    const float coolant=clampf(72.0f+
        29.0f*std::sin(3.0f*phase-0.35f)+
        12.0f*std::sin(6.0f*phase+0.4f),25.0f,115.0f);
    const float soc=clampf(55.0f+
        30.0f*std::cos(phase-0.3f)+
        8.0f*std::sin(5.0f*phase+0.2f),12.0f,95.0f);
    DemoValues v{};
    v.rpm=static_cast<uint16_t>(std::lround(clampf(rpm,0.0f,6500.0f)));
    v.speed_kmh=static_cast<uint8_t>(std::lround(clampf(speed,0.0f,160.0f)));
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
