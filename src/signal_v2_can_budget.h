#pragma once
#include "../include/signal_v2_vehicle_profile.h"
#include <array>
#include <cstdint>
namespace signal_v2 {
// One extended CAN 8-byte frame costs <=200 conservative bus bits including
// worst-case stuffing/interframe. Reserve TX, expected RX, ISO-TP FC together.
// Sliding windows, not resettable aligned buckets.
class CanBudget {
  struct Reservation {uint32_t at=0;uint32_t bits=0;uint16_t duration=0;bool used=false;};
  std::array<Reservation,64> journal_{};
  uint32_t last_=0,saturated_at_=0;
  bool saturated_=false;
public:
  uint32_t peak100=0,peak1000=0,denied=0,used100=0,used1000=0;
  // Max 256 source addresses, each may send separate RPM and speed SFs.
  // A broadcast cannot be admitted under a 5000-bit/100ms cap.
  static constexpr uint16_t DiscoveryFrames = 1 + 256*2;
  // N1 physical probe: TX 1 + RX <=2 Single Frames + 1 safety allowance.
  static constexpr uint16_t ProbeFrames = profiles::kLegacyRp8.probe_frames;
  void refresh(uint32_t now) {
    if(saturated_ && uint32_t(now-saturated_at_)<1000){
      used100=used1000=UINT32_MAX;return;
    }
    saturated_=false;
    used100=used1000=0;
    for(const auto &r:journal_)if(r.used){
      const uint32_t age=now-r.at;
      if(age<uint32_t(100)+r.duration)used100+=r.bits;
      if(age<uint32_t(1000)+r.duration)used1000+=r.bits;
    }
  }
  // Actual frames remain in the journal even when the cap is exceeded.
  // On storage exhaustion UINT32_MAX explicitly means saturated/unknown,
  // never a deceptively low usage. TX is latched off by the caller.
  bool observe(uint32_t now){
    for(auto &r:journal_)if(!r.used || uint32_t(now-r.at)>=1000){
      r.at=now;r.bits=profiles::kLegacyRp8.bits_per_frame;r.duration=0;r.used=true;
      refresh(now);
      if(used100>peak100)peak100=used100;
      if(used1000>peak1000)peak1000=used1000;
      return used100<=profiles::kLegacyRp8.bits_100ms && used1000<=profiles::kLegacyRp8.bits_1s;
    }
    saturated_=true;saturated_at_=now;
    used100=used1000=peak100=peak1000=UINT32_MAX;
    return false;
  }
  bool permits(uint32_t now,uint16_t frames){
    refresh(now);
    if(uint64_t(used100)+uint32_t(frames)*profiles::kLegacyRp8.bits_per_frame>profiles::kLegacyRp8.bits_100ms ||
       uint64_t(used1000)+uint32_t(frames)*profiles::kLegacyRp8.bits_per_frame>profiles::kLegacyRp8.bits_1s){++denied;return false;}
    return true;
  }
  bool reserve(uint32_t now,uint16_t worst_frames,uint16_t duration=0){
    if(last_&&now<last_){++denied;return false;}
    last_=now;
    refresh(now);
    const uint32_t a=used100,b=used1000;
    const uint32_t cost=uint32_t(worst_frames)*profiles::kLegacyRp8.bits_per_frame;
    if(uint64_t(a)+cost>profiles::kLegacyRp8.bits_100ms||uint64_t(b)+cost>profiles::kLegacyRp8.bits_1s){++denied;return false;}
    for(auto &r:journal_)if(!r.used||now-r.at>=uint32_t(1000)+r.duration){
      r.at=now;r.bits=cost;r.duration=duration;r.used=true;
      used100=a+cost;used1000=b+cost;
      if(used100>peak100)peak100=used100;
      if(used1000>peak1000)peak1000=used1000;
      return true;
    }
    ++denied;return false;
  }
};
} // namespace signal_v2
