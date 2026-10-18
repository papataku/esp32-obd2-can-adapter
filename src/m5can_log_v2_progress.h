#pragma once
// Wrap-safe V2 sender watchdog. Only advancing verified SD ACKs or
// successful BLE enqueue refresh the timer. Duplicate ACKs are not progress.
#include <cstdint>
namespace m5can_log_stream_v2 {
class ProgressWatchdog {
 public:
  void reset(uint32_t now_ms) { last_ms_=now_ms; }
  void noteNotification(uint32_t now_ms) { last_ms_=now_ms; }
  bool noteAck(uint32_t before,uint32_t after,uint32_t now_ms) {
    if(after<=before)return false;
    last_ms_=now_ms;
    return true;
  }
  bool elapsed(uint32_t now_ms,uint32_t limit_ms)const {
    return static_cast<uint32_t>(now_ms-last_ms_)>=limit_ms;
  }
 private:
  uint32_t last_ms_=0;
};
// A stalled CAN-owner mailbox must not keep RAW transfer active forever.
// arm() is idempotent so retries cannot postpone the timeout.
class BlockFetchWatchdog {
 public:
  static constexpr uint32_t kTimeoutMs=5000;
  void arm(uint32_t now_ms) {
    if(!armed_) { start_ms_=now_ms; armed_=true; }
  }
  void clear() { armed_=false; }
  bool expired(uint32_t now_ms) const {
    return armed_ && static_cast<uint32_t>(now_ms-start_ms_)>=kTimeoutMs;
  }
 private:
  uint32_t start_ms_=0;
  bool armed_=false;
};
} // namespace m5can_log_stream_v2
