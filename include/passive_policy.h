#pragma once
#include <cstddef>
#include <cstdint>
namespace passive {
constexpr bool kCanTransmit = false;
constexpr bool kCanArm = false;
constexpr bool kCanLease = false;
constexpr uint32_t kCheckPeriodMs = 100;
constexpr uint32_t kMaxCheckGapMs = 200;
constexpr unsigned kTxd = 13, kRx = 15, kInstallSink = 39;
static_assert(!kCanTransmit && !kCanArm && !kCanLease, "passive is immutable");
static_assert(kInstallSink != kTxd && kInstallSink != kRx, "TX sink must be separate");
struct Snapshot {
  bool no_tx_routes = false, txd_gpio = false, txd_enabled = false;
  bool txd_latch_high = false, txd_pad_high = false, txd_mux_gpio = false;
  bool txd_input_enabled = false, txd_push_pull = false, txd_awake = false;
  bool rx_route = false, controller_listen_only = false, running = false;
};
inline bool safe(const Snapshot& s) {
  return s.no_tx_routes && s.txd_gpio && s.txd_enabled &&
      s.txd_latch_high && s.txd_pad_high && s.txd_mux_gpio &&
      s.txd_input_enabled && s.txd_push_pull && s.txd_awake &&
      s.rx_route && s.controller_listen_only && s.running;
}
class Latch {
 public:
  bool check(const Snapshot& s, uint32_t now) {
    if (fault_) return false;
    if (!safe(s) || (verified_ && uint32_t(now-last_) > kMaxCheckGapMs)) {
      fault_ = true; verified_ = false; return false;
    }
    last_ = now; verified_ = true; return true;
  }
  bool verified(uint32_t now) const {
    return verified_ && !fault_ && uint32_t(now-last_) <= kMaxCheckGapMs;
  }
  void fail() { fault_ = true; verified_ = false; }
  bool fault() const { return fault_; }
 private:
  uint32_t last_ = 0;
  bool verified_ = false, fault_ = false;
};
}  // namespace passive
