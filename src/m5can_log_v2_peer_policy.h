#pragma once
// Opt-in V2 scheduling policy: an authenticated log subscriber remains
// a valid session while ordinary ELM replies occupy its transmission queue.
// All BLE connection, encryption and bonding checks remain in the transport.
namespace m5can_log_stream_v2 {
struct PeerQosPolicy {
  // Keep individual BLE host polls bounded. Three 244B notifications (~732B)
  // can fill the existing per-peer controller queue, but do not wait inside
  // a poll; the next loop always serves normal ELM/PID output first.
  // notifyCredit() and all two-peer checks run again for EACH frame.
  static constexpr unsigned kMaxLogFramesPerPoll=3;
  static_assert(kMaxLogFramesPerPoll<=3,
                "V2 burst must not exceed the bounded host credit budget");
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
  // The application ELM FIFO can be empty while older bulk frames remain
  // queued in NimBLE/controller FIFO ahead of the NEXT PID response. With
  // two peers keep at most one RAW notification submitted but not completed.
  // Called only after notifyCredit() validated the real peer/host snapshot;
  // single-peer preloading and ASCII notification credit remain unchanged.
  static bool rawHostCredit(unsigned active_peers,unsigned host_pending,
                            unsigned host_queued) {
    return active_peers<2 || (host_pending==0 && host_queued==0);
  }
  static bool maySend(bool eligible_peer,bool all_elm_queues_empty) {
    return eligible_peer && all_elm_queues_empty;
  }
};
}
