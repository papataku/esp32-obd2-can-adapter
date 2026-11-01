#pragma once
#include "signal_v2_vehicle_profile.h"
#include <array>
#include <cstddef>
#include <cstdint>
namespace signal_v2 {
// A bounded, host-testable source. Only a fully decoded physical response
// from one pinned ECU may populate these samples.
struct CanSamples {
  static constexpr uint16_t Pids[5]={profiles::kLegacyRp8.pids[0],profiles::kLegacyRp8.pids[1],
    profiles::kLegacyRp8.pids[2],profiles::kLegacyRp8.pids[3],profiles::kLegacyRp8.pids[4]};
  static constexpr uint8_t Lengths[5]={3,4,3,3,8};
  struct Sample {
    uint8_t raw[8]{};
    uint8_t length=0,quality=2; // NO_DATA by default.
    uint32_t sequence=0;
    uint64_t observed_us=0;
  };
  std::array<Sample,5> values{};
  uint32_t source_id=0,acquired=0,invalidations=0;
  uint32_t source_generation=1,mode_epoch=1;
  const Sample* sample(uint16_t pid) const {
    for(unsigned i=0;i<5;++i)if(Pids[i]==pid)return &values[i];
    return nullptr;
  }
  void invalidate(uint8_t reason=2){
    for(auto &s:values){s.length=0;s.quality=reason;s.observed_us=0;}
    ++invalidations;
  }
  static bool decodeGroup(const uint8_t* data,size_t len,bool full,
                          std::array<Sample,5>& target){
    if(!data||len<2||data[0]!=0x41)return false;
    size_t pos=1;
    for(unsigned i=0;i<(full?5U:2U);++i){
      unsigned j=full?i:(i?0:1); // full starts 0D; essential starts 0C
      const uint8_t bytes=Lengths[j];
      if(pos+bytes-1>len||data[pos]!=Pids[j])return false;
      target[j].length=bytes;
      target[j].raw[0]=0x41;target[j].raw[1]=data[pos];
      for(unsigned k=2;k<bytes;++k)target[j].raw[k]=data[pos+k-1];
      pos+=bytes-1;
    }
    return pos==len; // never admit trailing/unrequested/duplicate PIDs.
  }
};
} // namespace signal_v2
