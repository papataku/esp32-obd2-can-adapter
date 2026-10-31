#include "signal_v2_can_samples.h"
namespace signal_v2 {
// C++11 Xtensa toolchain requires one definition for ODR-used arrays.
constexpr uint16_t CanSamples::Pids[5];
constexpr uint8_t CanSamples::Lengths[5];
}
