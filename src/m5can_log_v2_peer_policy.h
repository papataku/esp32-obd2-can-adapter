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
  static bool maySend(bool eligible_peer,bool elm_queue_empty) {
    return eligible_peer && elm_queue_empty;
  }
};
}
