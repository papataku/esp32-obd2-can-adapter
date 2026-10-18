#include "../src/m5can_log_v2_peer_policy.h"
#include <cassert>
#include <iostream>
using m5can_log_stream_v2::PeerQosPolicy;
int main() {
  const bool eligible=PeerQosPolicy::eligible(true,true,true,true,false);
  assert(eligible);
  assert(PeerQosPolicy::maySend(eligible,true));
  // A CAN/PID / ELM reply temporarily occupying the queue defers RAW,
  // but MUST NOT revoke the previously verified V2 subscription.
  assert(!PeerQosPolicy::maySend(eligible,false));
  assert(PeerQosPolicy::eligible(true,true,true,true,false));
  assert(PeerQosPolicy::maySend(eligible,true));
  // Disconnect, unsubscribe, loss of peer authorization or negotiated MTU
  // are permanent per-session failures, unlike transient queue pressure.
  assert(!PeerQosPolicy::eligible(false,true,true,true,false));
  assert(!PeerQosPolicy::eligible(true,false,true,true,false));
  assert(!PeerQosPolicy::eligible(true,true,false,true,false));
  assert(!PeerQosPolicy::eligible(true,true,true,false,false));
  assert(!PeerQosPolicy::eligible(true,true,true,true,true));
  assert(!PeerQosPolicy::maySend(false,true));
  std::cout << "V2 ELM priority without spurious log session abort PASS\n";
}
