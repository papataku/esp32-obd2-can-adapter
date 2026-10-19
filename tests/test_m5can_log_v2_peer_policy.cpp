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
  // A second active BLE peer also has priority over 244B RAW bulk
  // notifications, even if the RAW receiver's ELM queue is empty.
  const auto both_idle=[](bool first_active,unsigned first_pending,
                          bool second_active,unsigned second_pending) {
    return PeerQosPolicy::elmQueueIdle(first_active,first_pending) &&
           PeerQosPolicy::elmQueueIdle(second_active,second_pending);
  };
  assert(both_idle(true,0,true,0));
  assert(!both_idle(true,0,true,1));
  assert(!both_idle(true,1,true,0));
  assert(!PeerQosPolicy::maySend(eligible,both_idle(true,0,true,3)));
  assert(!PeerQosPolicy::maySend(eligible,both_idle(true,5,true,0)));
  assert(PeerQosPolicy::maySend(eligible,both_idle(true,0,true,0)));
  // Stale pending counts in a disconnected client slot cannot starve V2.
  assert(both_idle(true,0,false,42));

  // Deterministic TWO-peer scheduling simulation, not an on-air RF test.
  // Serve one ELM notification per active client before bulk V2, each tick.
  unsigned meter_pending=0,other_pending=0;
  unsigned meter_replies=0,other_replies=0,v2_frames=0;
  unsigned v2_blocked_by_other=0;
  for(unsigned tick=0;tick<10000;++tick) {
    if(tick%5==0)meter_pending+=2;
    if(tick%7==0)other_pending+=3;
    if(meter_pending) {--meter_pending;++meter_replies;}
    if(other_pending) {--other_pending;++other_replies;}
    const bool idle=both_idle(true,meter_pending,true,other_pending);
    const bool send=PeerQosPolicy::maySend(eligible,idle);
    if(other_pending && meter_pending==0) {
      assert(!send);
      ++v2_blocked_by_other;
    }
    if(send) {
      assert(!meter_pending && !other_pending);
      ++v2_frames;
    }
  }
  assert(meter_replies>=3900 && other_replies>=4200);
  assert(v2_blocked_by_other>1000 && v2_frames>1000);
  assert(PeerQosPolicy::eligible(true,true,true,true,false));
  std::cout << "V2 two-peer ELM-first scheduling simulated PASS\n";
  std::cout << "V2 ELM priority without spurious log session abort PASS\n";
}
