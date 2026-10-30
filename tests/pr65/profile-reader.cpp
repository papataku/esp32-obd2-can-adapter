#include "signal_v2_discovery_profile.h"
uint32_t read_profile(const signal_v2::DiscoveryCandidate* p){return p[0].response_id;}
