#pragma once
// Pure C++ touch hit-testing for the M5Dial DEMO AUTO/OFF soft key.
// The state changes once per press, never on hold / touch movement.
#include <cstdint>

namespace m5can {
class DemoTouchControl {
 public:
  static constexpr int kLeft=38;
  static constexpr int kRight=202;
  static constexpr int kTop=50;
  static constexpr int kBottom=71;
  static constexpr uint32_t kDebounceMs=400;

  bool onPress(int x,int y,uint32_t now_ms) {
    if (x<kLeft || x>=kRight || y<kTop || y>=kBottom) return false;
    if (has_last_press_ &&
        static_cast<uint32_t>(now_ms-last_press_ms_)<kDebounceMs) return false;
    has_last_press_=true;
    last_press_ms_=now_ms;
    return true;
  }
 private:
  bool has_last_press_=false;
  uint32_t last_press_ms_=0;
};
} // namespace m5can
