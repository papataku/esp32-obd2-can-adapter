#pragma once
#include "signal_v2_vehicle_profile.h"
#include <cstdint>
namespace signal_v2 {
// N1 PR-B: owner-approved, evidence-backed physical discovery candidates.
// docs/protocol/signal-v2-2-bounded-ecu-discovery-pr-a.md section 1.
// No functional broadcast, no ID sweep, no NVS hint; unknown vehicle = empty.
// Aggregate without member initializers: firmware builds as C++11.
struct DiscoveryCandidate {
  CanIdWidth width;
  uint32_t response_id;
  uint32_t request_id;
  EcuRoute route()const{
    EcuRoute r{};r.width=width;r.response_id=response_id;r.request_id=request_id;
    return r;
  }
};
struct DiscoveryProfile {
  static constexpr uint8_t kMaxCandidates=4;
#if defined(M5CAN_SIGNAL_V2_RP8_PROFILE)
  // RP8 provisional source preference; other observed responders are
  // deliberately NOT candidates until a separately approved profile revision.
  static constexpr uint8_t kCount=profiles::kLegacyRp8.candidate_count;
  static constexpr DiscoveryCandidate kCandidates[kMaxCandidates]={
    {profiles::kLegacyRp8.width,profiles::kLegacyRp8.responder,profiles::kLegacyRp8.request_id},{CanIdWidth::Unknown,0,0},
    {CanIdWidth::Unknown,0,0},{CanIdWidth::Unknown,0,0}};
#else
  static constexpr uint8_t kCount=0;
  static constexpr DiscoveryCandidate kCandidates[kMaxCandidates]={
    {CanIdWidth::Unknown,0,0},{CanIdWidth::Unknown,0,0},
    {CanIdWidth::Unknown,0,0},{CanIdWidth::Unknown,0,0}};
#endif
  static_assert(kCount<=kMaxCandidates,"N1 caps physical discovery at 4");
  // C++11 constexpr: single-return recursion over candidates [i, kCount).
  static constexpr bool unique(uint8_t i,uint8_t j){
    return j>=i || (kCandidates[j].response_id!=kCandidates[i].response_id &&
                    unique(i,uint8_t(j+1)));
  }
  static constexpr bool wellFormed(uint8_t i=0){
    return i>=kCount ||
      (kCandidates[i].width==CanIdWidth::Extended29 &&
       (kCandidates[i].response_id&0xFFFFFF00U)==0x18DAF100U &&
       unique(i,0) && wellFormed(uint8_t(i+1)));
  }
};
static_assert(DiscoveryProfile::wellFormed(),
              "29bit-only trial; unique 18DAF1xx candidates");
} // namespace signal_v2
