#include "passive_policy.h"
#include <cassert>
#include <iostream>
using namespace passive;
int main() {
  Snapshot s{true,true,true,true,true,true,true,true,true,true,true,true};
  assert(safe(s));
  // Independently inject every observed hardware/state failure. Every failure
  // must be sticky even after the hardware becomes healthy again.
  bool Snapshot::* fields[]={&Snapshot::no_tx_routes,&Snapshot::txd_gpio,
    &Snapshot::txd_enabled,&Snapshot::txd_latch_high,&Snapshot::txd_pad_high,
    &Snapshot::txd_mux_gpio,&Snapshot::txd_input_enabled,&Snapshot::txd_push_pull,
    &Snapshot::txd_awake,&Snapshot::rx_route,&Snapshot::controller_listen_only,&Snapshot::running};
  for (auto field:fields) {
    Latch l; assert(!l.verified(0)); assert(l.check(s,100));
    Snapshot bad=s; bad.*field=false;
    assert(!safe(bad)); assert(!l.check(bad,200)); assert(l.fault());
    assert(!l.verified(200)); assert(!l.check(s,300));
  }
  Latch timing; assert(timing.check(s,0)); assert(timing.check(s,100));
  assert(timing.verified(300)); assert(!timing.verified(301));
  assert(!timing.check(s,301)); assert(timing.fault());
  Latch wrap; assert(wrap.check(s,UINT32_MAX-50));
  assert(wrap.check(s,49)); assert(wrap.verified(149));
  wrap.fail(); assert(!wrap.check(s,150));
  static_assert(!kCanTransmit && !kCanArm && !kCanLease);
  std::cout << "PASS passive hardware fault injection, stale check, wrap and sticky latch\n";
}
