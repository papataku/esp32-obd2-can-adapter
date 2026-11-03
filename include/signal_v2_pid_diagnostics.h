#pragma once
#include <array>
#include <cstdint>
#include <climits>
namespace signal_v2 {
// Owner-task-only, cumulative read-only telemetry. Never an admission input.
// Order: 0D, 0C, 05, 5B, 9A. Probe traffic is not instrument completion.
struct PidDiagnostics {
  struct Count {
    uint64_t demand=0, admitted=0, completed=0, shed=0, denied=0, failed=0, cancelled=0;
  };
  std::array<Count,5> pid{};
  uint64_t active_ms=0;
  uint32_t last=0, due=0, epoch=0;
  bool active=false, pending=false, essential=false;
  // Same merged-period floor as CanOwner::tick (profiles::kLegacyRp8
  // period_5hz_ms / period_10hz_ms); tests/pid_s1.cpp static_asserts equality.
#if defined(M5CAN_SIGNAL_V2_10HZ_LAB)
  static constexpr uint16_t kPeriodFloorMs=100;
#else
  static constexpr uint16_t kPeriodFloorMs=200;
#endif
  static void add(uint64_t &v,uint64_t n=1) { v=UINT64_MAX-v<n?UINT64_MAX:v+n; }
  void cancel() {
    if(pending)for(unsigned i=0;i<(essential?2U:5U);++i)add(pid[i].cancelled);
    pending=false;
  }
  template<class Demand> void observe(uint32_t now,const Demand &d,bool enabled,uint32_t e) {
    uint16_t period=10000; bool any=false;
    for(auto p:d)if(p){any=true;if(p<period)period=p;}
    if(period<kPeriodFloorMs)period=kPeriodFloorMs;
#if defined(M5CAN_SIGNAL_V2_STAGE_GATED)
    enabled=false; // staged observation never represents LIVE gauge demand
#endif
    const bool on=enabled&&any;
    if(e!=epoch||!enabled)cancel();
    if(active)add(active_ms,uint32_t(now-last));
    if(!active||e!=epoch)due=now;
    if(on&&int32_t(now-due)>=0){
      const uint32_t n=uint32_t(now-due)/period+1;
      for(auto &c:pid)add(c.demand,n);
      due+=n*period;
    }
    last=now;epoch=e;active=on;
  }
  void admit(bool small) {
    pending=true;essential=small;
    for(unsigned i=0;i<(small?2U:5U);++i)add(pid[i].admitted);
  }
  void finish(bool valid) {
    if(pending)for(unsigned i=0;i<(essential?2U:5U);++i){
      if(valid)add(pid[i].completed);else add(pid[i].failed);
    }
    pending=false;
  }
  void shedAux() { for(unsigned i=2;i<5;++i)add(pid[i].shed); }
  void deny(bool small) { for(unsigned i=0;i<(small?2U:5U);++i)add(pid[i].denied); }
};
} // namespace signal_v2
