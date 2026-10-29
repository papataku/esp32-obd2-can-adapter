#include "signal_v2_discovery_profile.h"

namespace signal_v2 {
// C++11 requires storage when the constexpr array is ODR-used.
constexpr DiscoveryCandidate DiscoveryProfile::kCandidates[DiscoveryProfile::kMaxCandidates];
}  // namespace signal_v2
