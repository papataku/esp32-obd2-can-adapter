#include "signal_v2_discovery_profile.h"
extern uint32_t read_profile(const signal_v2::DiscoveryCandidate*);
int main(){return read_profile(signal_v2::DiscoveryProfile::kCandidates)==0x18DAF101U?0:1;}
