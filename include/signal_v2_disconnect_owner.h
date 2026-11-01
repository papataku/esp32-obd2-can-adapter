#pragma once
#include <cstdint>
namespace signal_v2 {
// Protected by the GATT link mux. Unknown during every Session mutation.
struct DisconnectOwner {
  bool known = false;
  uint16_t handle = 0;
  uint32_t generation = 0, epoch = 0, deadline = 0, session = 0;
  bool current(uint32_t e, uint32_t now) const {
    return known && epoch == e && int32_t(deadline - now) > 0;
  }
};
} // namespace signal_v2
