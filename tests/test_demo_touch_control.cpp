#include <cassert>
#include <cstdint>
#include "demo_touch_control.h"
int main() {
  using m5can::DemoTouchControl;
  DemoTouchControl t{};
  assert(!t.onPress(37,60,1000));
  assert(!t.onPress(202,60,1000));
  assert(!t.onPress(120,49,1000));
  assert(!t.onPress(120,71,1000));
  assert(t.onPress(38,50,1000));
  assert(!t.onPress(120,60,1300));  // contact bounce/repeat
  assert(t.onPress(201,70,1400));
  assert(t.onPress(120,60,1800));
  // Wrap-safe debounce with ESP32 millis rollover.
  DemoTouchControl rolled{};
  assert(rolled.onPress(120,60,0xffffff00UL));
  assert(!rolled.onPress(120,60,0xffffff10UL));
  assert(rolled.onPress(120,60,0x000000a0UL));
}
