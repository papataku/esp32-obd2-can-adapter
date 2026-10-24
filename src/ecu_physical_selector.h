#pragma once
#include <cstdint>
#include <cstring>
#include "adaptive_multi_pid.h"

namespace m5can {
// Issue #54. One bounded controller shared by both BLE clients (CPU1 owner).
// No timer or startup activity: one functional probe only on a real demand.
class EcuPhysicalSelector {
 public:
  enum class State : uint8_t { Discover, Fixed, Recheck, Legacy };
  static constexpr uint8_t kProbeLimit=8;
  static constexpr uint8_t kFailureLimit=3;
  struct Candidate {
    uint32_t can_id=0;
    uint32_t successes=0,changes=0;
    uint64_t latency_us=0;
    uint16_t last_rpm=0;
    uint8_t last_speed=0;
    bool previous=false,disqualified=false;
  };
  State state() const { return state_; }
  bool probing() const { return state_==State::Discover || state_==State::Recheck; }
  bool fixed() const { return state_==State::Fixed; }
  bool legacy() const { return state_==State::Legacy; }
  uint32_t chosenResponse() const { return chosen_; }
  uint32_t physicalId() const {
    return chosen_ ? 0x18DA00F1U | ((chosen_&0xFFU)<<8) : 0;
  }
  uint8_t probes() const { return probes_; }
  uint8_t retries() const { return retries_; }
  uint8_t candidates() const { return used_; }
  const Candidate& candidate(uint8_t i) const { return candidates_[i]; }
  uint32_t physicalOk() const { return physical_ok_; }
  uint32_t physicalFailed() const { return physical_failed_; }
  uint64_t rawRxFrames() const { return raw_rx_frames_; }
  void disable() { state_=State::Legacy; }
#if defined(M5CAN_VEHICLE_HINT_EXPERIMENTAL)
  bool restoredNeedsPhysicalProof() const { return restored_unconfirmed_; }
  // Called only after an exact current-boot functional CAN/RAW fingerprint
  // matched the saved advisory hint. Never authorize TX based on NVS alone.
  bool restoreAfterFunctionalProof(const DiagnosticResult& result,
                                   uint32_t response_id,
                                   DiagnosticRawFrame frames[2]) {
    if(state_!=State::Discover || probes_!=0 ||
       !DiagnosticPolicy::observedPhysicalEcu(
          0x18DA00F1U|((response_id&0xFFU)<<8)) ||
       !decode(result,response_id,frames))return false;
    chosen_=response_id;
    state_=State::Fixed;
    restored_unconfirmed_=true;
    return true;
  }
#endif
  // Single-parameter reads to the same pinned ECU share failure accounting.
  void observeSingle(bool valid,DiagnosticStatus status) {
    if(!fixed())return;
    if(valid){
      ++physical_ok_;consecutive_bad_=0;
#if defined(M5CAN_VEHICLE_HINT_EXPERIMENTAL)
      restored_unconfirmed_=false;
#endif
      return;
    }
#if defined(M5CAN_VEHICLE_HINT_EXPERIMENTAL)
    if(restored_unconfirmed_) {
      ++physical_failed_;
      resetAfterUnconfirmedHint();
      return;
    }
#endif
    markFailure(status);
  }

  static bool parsePair(const DiagnosticMessage& m,uint16_t& rpm,uint8_t& speed) {
    if ((m.can_id&0x1FFFFF00U)!=0x18DAF100U ||
        m.length!=6 || m.data[0]!=0x41) return false;
    if (m.data[1]==0x0C && m.data[4]==0x0D) {
      rpm=(uint16_t(m.data[2])<<8)|m.data[3];
      speed=m.data[5]; return true;
    }
    if (m.data[1]==0x0D && m.data[3]==0x0C) {
      speed=m.data[2];
      rpm=(uint16_t(m.data[4])<<8)|m.data[5]; return true;
    }
    return false;
  }
  static bool decode(const DiagnosticResult& r,uint32_t source,
                     DiagnosticRawFrame out[2]) {
    if(!source || r.status!=DiagnosticStatus::Ok)return false;
    AdaptiveMultiPid cap;
    cap.markSupported(source);
    uint32_t got=0;
    return cap.decode(r,out,got) && got==source;
  }
  // Exactly one call per completed grouped CAN transaction.
  // physical=true ONLY for a submitted 18DAxxF1 group.
  bool observe(const DiagnosticResult& r,bool physical,DiagnosticRawFrame out[2]) {
    raw_rx_frames_+=r.raw_frame_count;
    if(legacy())return false;
    if(physical) {
      if(!fixed())return false;
      if(decode(r,chosen_,out)) {
        ++physical_ok_;consecutive_bad_=0;
#if defined(M5CAN_VEHICLE_HINT_EXPERIMENTAL)
        restored_unconfirmed_=false;
#endif
        return true;
      }
#if defined(M5CAN_VEHICLE_HINT_EXPERIMENTAL)
      if(restored_unconfirmed_) {
        ++physical_failed_;
        resetAfterUnconfirmedHint();
        return false;
      }
#endif
      markFailure(r.status);
      return false;
    }
    if(!probing())return false;
    ++probes_;
    uint32_t present[kDiagnosticMaxResponses]{};
    uint8_t present_count=0;
    if(r.status==DiagnosticStatus::Ok && r.response_count<=kDiagnosticMaxResponses) {
      for(uint8_t i=0;i<r.response_count;++i) {
        const auto& m=r.responses[i];
        if((m.can_id&0x1FFFFF00U)!=0x18DAF100U)continue;
        Candidate* c=entry(m.can_id);
        if(!c)continue;
        bool duplicate=false;
        for(uint8_t j=0;j<present_count;++j)
          if(present[j]==m.can_id)duplicate=true;
        if(duplicate) {c->disqualified=true;continue;}
        present[present_count++]=m.can_id;
        uint16_t rpm=0;uint8_t speed=0;
        if(!parsePair(m,rpm,speed) || !matchingRaw(r,m)) {
          c->disqualified=true;continue;
        }
        ++c->successes;
        if(c->previous && (c->last_rpm!=rpm || c->last_speed!=speed))
          ++c->changes;
        c->last_rpm=rpm;c->last_speed=speed;c->previous=true;
        c->latency_us+=responseLatency(r,m.can_id);
      }
    }
    // Provisional display source for this single observation, not a
    // persistent choice; never mix ECU sources in one delivered pair.
    uint32_t provisional=0;
    for(uint8_t i=0;i<used_;++i) {
      const auto& c=candidates_[i];
      bool present_now=false;
      for(uint8_t j=0;j<present_count;++j)
        if(present[j]==c.can_id)present_now=true;
      if(present_now && !c.disqualified &&
         (!provisional || c.can_id<provisional))provisional=c.can_id;
    }
    if(probes_==kProbeLimit)finalize();
    if(fixed())provisional=chosen_;
    return decode(r,provisional,out);
  }
 private:
#if defined(M5CAN_VEHICLE_HINT_EXPERIMENTAL)
  void resetAfterUnconfirmedHint() {
    // The saved hint was NOT a new full selection. Start fresh bounded
    // evaluation rather than freezing a potentially wrong ECU.
    restored_unconfirmed_=false;
    state_=State::Discover;
    chosen_=0;probes_=0;used_=0;consecutive_bad_=0;
    for(auto& candidate:candidates_)candidate=Candidate{};
  }
#endif
  void markFailure(DiagnosticStatus status) {
    ++physical_failed_;
    // No additional automatic probes on lease/driver/bus failures.
    if(status!=DiagnosticStatus::Ok && status!=DiagnosticStatus::NoData &&
       status!=DiagnosticStatus::IsoTpError) {disable();return;}
    if(++consecutive_bad_>=kFailureLimit){
      if(retries_!=0)disable();
      else {
        ++retries_;state_=State::Recheck;probes_=used_=0;
        for(auto& candidate:candidates_)candidate=Candidate{};
      }
    }
  }
  Candidate* entry(uint32_t id) {
    for(uint8_t i=0;i<used_;++i)
      if(candidates_[i].can_id==id)return &candidates_[i];
    if(used_>=kDiagnosticMaxResponses)return nullptr;
    candidates_[used_].can_id=id;
    return &candidates_[used_++];
  }
  static bool matchingRaw(const DiagnosticResult& r,const DiagnosticMessage& m){
    for(uint8_t j=0;j<r.raw_frame_count && j<kDiagnosticMaxRawFrames;++j) {
      const auto& f=r.raw_frames[j];
      if(f.can_id==m.can_id && f.dlc>=7 && f.data[0]==6 &&
         std::memcmp(&f.data[1],m.data,6)==0)return true;
    }
    return false;
  }
  static uint32_t responseLatency(const DiagnosticResult& r,uint32_t id){
    for(uint8_t j=0;j<r.raw_frame_count && j<kDiagnosticMaxRawFrames;++j)
      if(r.raw_frames[j].can_id==id)return r.raw_rx_offsets_us[j];
    return r.tx_to_first_response_us;
  }
  static bool better(const Candidate& a,const Candidate& b) {
    if(a.successes!=b.successes)return a.successes>b.successes;
    if(a.changes!=b.changes)return a.changes>b.changes;
    // Exact rational mean comparison.
    uint64_t al=a.latency_us*b.successes,bl=b.latency_us*a.successes;
    if(al!=bl)return al<bl;
    return a.can_id<b.can_id;
  }
  void finalize(){
    const Candidate* best=nullptr;
    for(uint8_t i=0;i<used_;++i){
      const auto& c=candidates_[i];
      if(c.disqualified || c.successes==0)continue;
      if(!best || better(c,*best))best=&c;
    }
    if(!best) {disable();return;}
    if(state_==State::Recheck && best->can_id!=chosen_) {
      disable();return; // never silently repin to another ECU this boot
    }
    if(state_==State::Discover)chosen_=best->can_id;
    state_=State::Fixed;consecutive_bad_=0;
  }
#if defined(M5CAN_VEHICLE_HINT_EXPERIMENTAL)
  bool restored_unconfirmed_=false;
#endif
  State state_=State::Discover;
  Candidate candidates_[kDiagnosticMaxResponses]{};
  uint32_t chosen_=0,physical_ok_=0,physical_failed_=0;
  uint64_t raw_rx_frames_=0;
  uint8_t used_=0,probes_=0,retries_=0,consecutive_bad_=0;
};
} // namespace m5can
