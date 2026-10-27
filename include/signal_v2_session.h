#pragma once
#include "signal_v2_codec.h"
#include "signal_v2_simulator.h"
namespace signal_v2 {
enum Result : uint8_t {
  Accept,
  Relaxed,
  Reject,
  BadRevision,
  BadSource,
  RateLimited,
  NotAuthorized,
  TxLocked,
  Busy,
  Unsupported,
  ProtocolError,
  ResultUnknown
};
class Session {
  struct History {
    Packet request, ack;
    bool used = false;
  };
  std::array<History, 8> history_{};
  size_t cursor_ = 0;
  uint32_t high_ = 0;
  uint32_t boot_ = 0, id_ = 0, revision_ = 0, deadline_ = 0, next_ = 0,
           frame_ = 0, sample_ = 0;
  bool hello_ = false, paused_ = true;
  uint8_t count_ = 0;
  std::array<uint8_t, 24 * 8> entries_{};
  std::array<uint8_t, 52 * 8> cache_{};
  std::array<uint8_t, 8> cache_lengths_{};
  std::array<uint32_t, 8> due_{};
  std::array<uint8_t, 200> plan_{};
  size_t plan_size_ = 0;
  uint32_t plan_id_ = 0, plan_deadline_ = 0;
  void apply(const uint8_t *, uint32_t);
  uint8_t cache_count_ = 0;
  size_t cache_size_ = 0;
  uint16_t period_ = 200;
  bool snapshot_ = false;
  uint32_t observed_ms_ = 0, heartbeat_ = 0;
  void ack(const Packet &, Result, const uint8_t *, size_t, Packet &);

public:
  void connect(uint32_t boot, uint32_t session);
  void invalidate() { connect(boot_, id_); }
  bool active(uint32_t now) const {
    return hello_ && !paused_ && count_ && int32_t(deadline_ - now) > 0;
  }
  uint32_t revision() const { return revision_; }
  uint32_t id() const { return id_; }
  uint8_t reservations(uint32_t now) const { return active(now) ? count_ : 0; }
  void control(const Packet &, bool authorized, uint32_t now, Packet &);
  void demand(uint32_t now, Simulator::Demand &) const;
  bool produce(uint32_t now, Packet &, const Simulator *source = nullptr);
};
} // namespace signal_v2
