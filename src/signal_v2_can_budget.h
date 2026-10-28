#pragma once
#include <array>
#include <cstdint>
namespace signal_v2 {
// One extended CAN 8-byte frame costs <=200 conservative bus bits including
// worst-case stuffing/interframe. Reserve TX, expected RX, ISO-TP FC together.
// Sliding windows, not resettable aligned buckets.
class CanBudget {
  struct Reservation {uint32_t at=0;uint16_t bits=0;bool used=false;};
  std::array<Reservation,64> journal_{};
  uint32_t last_=0;
public:
  uint32_t peak100=0,peak1000=0,denied=0,used100=0,used1000=0;
  bool reserve(uint32_t now,uint16_t worst_frames){
    if(last_&&now<last_){++denied;return false;}
    last_=now;
    uint32_t a=0,b=0;
    for(const auto &r:journal_)if(r.used){
      const uint32_t age=now-r.at;
      if(age<100)a+=r.bits;
      if(age<1000)b+=r.bits;
    }
    const uint32_t cost=uint32_t(worst_frames)*200;
    if(a+cost>5000||b+cost>25000){++denied;return false;}
    for(auto &r:journal_)if(!r.used||now-r.at>=1000){
      r={now,static_cast<uint16_t>(cost),true};
      used100=a+cost;used1000=b+cost;
      if(used100>peak100)peak100=used100;
      if(used1000>peak1000)peak1000=used1000;
      return true;
    }
    ++denied;return false;
  }
};
} // namespace signal_v2
