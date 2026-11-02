#pragma once
#include "signal_v2_codec.h"
#include "signal_v2_vehicle_profile.h"
#include <algorithm>
#include <cstring>
namespace signal_v2 {
// Acquisition reference only. No fingerprint bitmap/vehicle continuity exists
// in P3. An ACK is receipt of metadata, never profile approval or a TX grant.
struct BindingContext {
  uint32_t authorization_epoch=0, source_generation=0, mode_epoch=0;
  uint32_t acquisition=0, sequence=0, route=0;
  uint64_t observed_us=0;
  bool current_session_evidence=false;
};
class ProfileBinding {
  static constexpr size_t TupleSize=68;
  bool negotiated_=false, offered_=false, acknowledged_=false;
  std::array<uint8_t,TupleSize> offer_{};
  void tuple(uint32_t boot,uint32_t session,uint8_t *p) const {
    std::fill(p,p+TupleSize,0);
    w16(p,1);w16(p+2,profiles::kLegacyRp8.schema);
    w32(p+4,profiles::kLegacyRp8.revision);
    std::memcpy(p+8,profiles::kLegacyRp8.id,10);
    w32(p+24,boot);w32(p+28,session);
    w32(p+32,context.authorization_epoch);
    w32(p+36,context.source_generation);w32(p+40,context.mode_epoch);
    if(context.current_session_evidence && context.route==profiles::kLegacyRp8.responder &&
       context.acquisition && context.sequence && context.observed_us) {
      p[44]=1; // current-session legacy acquisition, NOT fingerprint match
      w32(p+48,context.acquisition);w32(p+52,context.sequence);
      w64(p+56,context.observed_us);w32(p+64,context.route);
    }
  }
public:
  BindingContext context;
  // Result values match Session::Result without introducing a header cycle.
  // This namespace is deliberately read-only. Legacy op2 stays RP8-only;
  // dynamic selection/activation has no command or authorization path in P3.
  uint8_t control(const uint8_t *b,size_t n,uint32_t boot,uint32_t session,
                  uint8_t *reply,size_t &reply_n) {
    if(n<4 || b[3])return 10; // ProtocolError
    if(u16(b)!=1)return 9; // Unsupported
    if(b[2]==0) {
      if(n!=4 || negotiated_)return 2; // Reject; no renegotiation/downgrade
      negotiated_=true;
      w16(reply,1);w16(reply+2,TupleSize);
      w32(reply+4,1); // bit0: metadata ACK; bit1 dynamic activation ABSENT
      reply_n=8;return 0;
    }
    if(!negotiated_)return 9;
    if(b[2]==1) {
      if(n!=4 || offered_ || acknowledged_)return 2;
      tuple(boot,session,offer_.data());offered_=true;
      std::copy(offer_.begin(),offer_.end(),reply);reply_n=TupleSize;return 0;
    }
    if(b[2]==2) {
      if(n!=4+TupleSize || !offered_ || acknowledged_)return 2;
      std::array<uint8_t,TupleSize> current{};
      tuple(boot,session,current.data());
      if(current!=offer_ || !std::equal(offer_.begin(),offer_.end(),b+4))return 2;
      acknowledged_=true;reply_n=TupleSize;
      std::copy(offer_.begin(),offer_.end(),reply);return 0;
    }
    return 9;
  }
};
} // namespace signal_v2
