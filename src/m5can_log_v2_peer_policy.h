#pragma once
// Opt-in V2 scheduling policy: an authenticated log subscriber remains
// a valid session while ordinary ELM replies occupy its transmission queue.
// All BLE connection, encryption and bonding checks remain in the transport.
namespace m5can_log_stream_v2 {
struct PeerQosPolicy {
  static bool eligible(bool active,bool subscribed,bool authorized,
                       bool mtu_ok,bool tx_aborted) {
    return active && subscribed && authorized && mtu_ok && !tx_aborted;
  }
  // The M5CAN BLE host/mbuf pool is shared by BOTH connected clients.
  // V2 can use leftover airtime only after every active peer's ordinary
  // ELM/CAN response queue has drained. A disconnected slot cannot starve
  // RAW forever just because its stale pending count has not been reset.
  // This is transient QoS, never a reason to revoke V2 authorization.
  static bool elmQueueIdle(bool active,unsigned pending_count) {
    return !active || pending_count==0;
  }
  static bool maySend(bool eligible_peer,bool all_elm_queues_empty) {
    return eligible_peer && all_elm_queues_empty;
  }
};
}
