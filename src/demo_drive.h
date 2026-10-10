#pragma once
// Bench-only simulated Honda e:HEV drive cycle. Never transmitted onto CAN.
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
};
class DemoDrive {
 public:
  static DemoValues sample(uint32_t elapsed_ms) {
    const float t = (elapsed_ms % 100000U) / 1000.0f;
    auto blend = [](float x, float a, float b, float from, float to) {
      const float p = std::max(0.0f, std::min(1.0f, (x - a) / (b - a)));
      return from + (to - from) * p * p * (3.0f - 2.0f * p);
    };
    float speed = 0, rpm = 0, power = 0;
    if (t < 5) { speed = 0; rpm = 0; power = 0.3f; }
    else if (t < 17) { speed = blend(t,5,17,0,42); power = blend(t,5,17,8,25); }
    else if (t < 30) {
      speed = blend(t,17,30,42,90);
      rpm = blend(t,17,30,1450,3500) + 90 * std::sin(t*1.3f);
      power = blend(t,17,30,27,63);
    } else if (t < 42) {
      speed = 90 + 2*std::sin(t*0.5f);
      rpm = t < 35 ? 0 : 1520 + 110 * std::sin(t*0.9f);
      power = 11 + 5*std::sin(t*0.8f);
    } else if (t < 55) {
      speed = blend(t,42,55,90,35);
      rpm = t < 44 ? blend(t,42,44,1500,0) : 0;
      power = -8 - 29*std::sin(3.14159265f*(t-42)/13);
    } else if (t < 63) {
      speed = blend(t,55,63,35,45); power = 5 + 7*std::sin((t-55)*0.4f);
    } else if (t < 76) {
      speed = blend(t,63,76,45,108);
      rpm = blend(t,63,76,1800,4300) + 130*std::sin(t*1.2f);
      power = blend(t,63,76,26,70);
    } else if (t < 90) {
      speed = blend(t,76,90,108,0);
      rpm = t < 78 ? blend(t,76,78,3500,0) : 0;
      power = -6 - 36*std::sin(3.14159265f*(t-76)/14);
    }
    DemoValues v{};
    v.rpm = static_cast<uint16_t>(std::lround(std::max(0.0f, rpm)));
    v.speed_kmh = static_cast<uint8_t>(std::lround(std::max(0.0f, speed)));
    v.power_kw = power;
    // Warm-up is independent of the 100-second driving cycle; later stays ~86C.
    const float warm = std::min(1.0f, elapsed_ms / 180000.0f);
    v.coolant_c = static_cast<int8_t>(std::lround(45 + 41*warm + 1.3f*std::sin(elapsed_ms/23000.0f)));
    // Slow net SOC movement, not rapid movement synced to RPM needle.
    const float soc_percent = 57.5f - 1.1f*std::sin(t*0.062831853f) -
                              0.8f*std::sin(t*0.125663706f);
    v.soc_raw = static_cast<uint8_t>(std::lround(soc_percent*255.0f/100.0f));
    return v;
  }

  // Generates actual ISO-TP-over-ELM frame shape, using the same response ID
  // as the 5 known RP8 OBD01 PIDs. No fabricated frames enter the TWAI driver.
  static uint8_t frames(uint8_t pid, uint32_t elapsed_ms, DiagnosticRawFrame out[2]) {
    if (!out) return 0;
    const DemoValues v = sample(elapsed_ms);
    out[0] = {}; out[1] = {};
    out[0].can_id = 0x18DAF101U;
    out[0].dlc = 8;
    if (pid == 0x0C) {
      const uint16_t raw = v.rpm * 4U;
      const uint8_t bytes[8] = {0x04,0x41,0x0C,
                              static_cast<uint8_t>(raw>>8),static_cast<uint8_t>(raw),0,0,0};
      for (int i=0;i<8;++i) out[0].data[i]=bytes[i];
      return 1;
    }
    if (pid == 0x0D || pid == 0x05 || pid == 0x5B) {
      const uint8_t value = pid == 0x0D ? v.speed_kmh :
         pid == 0x05 ? static_cast<uint8_t>(v.coolant_c+40) : v.soc_raw;
      const uint8_t bytes[8] = {0x03,0x41,pid,value,0,0,0,0};
      for (int i=0;i<8;++i) out[0].data[i]=bytes[i];
      return 1;
    }
    if (pid == 0x9A) {
      // Example RP8 electrical format: voltage /64, signed current /10.
      constexpr float voltage = 256.0f;
      const int16_t amps10 = static_cast<int16_t>(std::lround(v.power_kw * 10000.0f / voltage));
      const uint16_t signed_bits = static_cast<uint16_t>(amps10);
      const uint16_t voltage64 = static_cast<uint16_t>(voltage*64.0f);
      const uint8_t first[8] = {0x10,0x08,0x41,0x9A,0x07,0x00,
              static_cast<uint8_t>(voltage64>>8),static_cast<uint8_t>(voltage64)};
      const uint8_t next[8] = {0x21,static_cast<uint8_t>(signed_bits>>8),
                                    static_cast<uint8_t>(signed_bits),0,0,0,0,0};
      for (int i=0;i<8;++i) {out[0].data[i]=first[i];out[1].data[i]=next[i];}
      out[1].can_id = out[0].can_id; out[1].dlc = 8;
      return 2;
    }
    return 0;
  }
};
} // namespace m5can
