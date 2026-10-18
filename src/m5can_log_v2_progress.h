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
} // namespace m5can_log_stream_v2
