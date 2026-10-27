#pragma once
#include <cstdint>
namespace signal_v2 {
// After HELLO or profile change, wait for a validated SNAPSHOT before DATA.
// All DATA-lane frames including HEARTBEAT occupy the same sequence space.
inline bool acceptsSequential(uint32_t last, uint32_t current,
                              bool awaitingSnapshot) {
  return !awaitingSnapshot && last != 0 && current != 0 &&
         last != UINT32_MAX && current == last + 1;
}
} // namespace signal_v2
