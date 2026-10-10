#pragma once

#include <array>
#include <cstdint>

#include "diagnostic_policy.h"

namespace m5can {

// Space the *same diagnostic parameter*, not all parameters sharing a
// functional CAN arbitration ID. Mode01 010C/010D/019A/0105/015B all use
// 0x18DB33F1 on Honda RP8; they are distinct requests and must not
// inherit each other's 50ms delay.
//
// Every request is serialized by the CAN owner. The fixed, allocation-free
// table holds recently sent parameter keys. A full table never evicts an
// unexpired key: wait for a slot instead of silently bypassing the limit.
// ISO-TP flow-control frames bypass this class entirely.
class DiagnosticRequestPacer {
 public:
  static constexpr uint64_t kMinSameIdIntervalUs = 50000ULL;
  static constexpr uint64_t kMinSameParameterIntervalUs = kMinSameIdIntervalUs;
  static constexpr size_t kSlots = 32;

  uint64_t remainingUs(const DiagnosticRequest& request,
                       uint64_t now_us) const {
    if (!trackable(request)) return 0;
    const uint64_t key = requestKey(request);
    bool has_free_slot = false;
    uint64_t minimum_occupied_delay = kMinSameParameterIntervalUs;

    for (const Entry& entry : entries_) {
      if (!entry.valid) {
        has_free_slot = true;
        continue;
      }
      const uint64_t remaining = timeRemaining(entry.sent_us, now_us);
      if (entry.key == key) return remaining;
      if (remaining == 0) {
        has_free_slot = true;
      } else if (remaining < minimum_occupied_delay) {
        minimum_occupied_delay = remaining;
      }
    }
    // A new parameter is immediately eligible, even under the same CAN ID.
    // When all slots are occupied by recent TX, pause until safe to reuse.
    return has_free_slot ? 0 : minimum_occupied_delay;
  }

  // Call ONLY after a successful CAN transmission, never after a failed
  // attempt or for unapproved requests. Caller must first check remainingUs.
  void noteSuccessfulTx(const DiagnosticRequest& request,
                        uint64_t timestamp_us) {
    if (!trackable(request)) return;
    const uint64_t key = requestKey(request);
    Entry* reusable = nullptr;
    for (Entry& entry : entries_) {
      if (entry.valid && entry.key == key) {
        entry.sent_us = timestamp_us;
        return;
      }
      if (!reusable &&
          (!entry.valid || timeRemaining(entry.sent_us, timestamp_us) == 0)) {
        reusable = &entry;
      }
    }
    // If the caller ignored remainingUs and the table is full, preserve all
    // active histories rather than accidentally erasing a live throttle.
    if (!reusable) return;
    reusable->key = key;
    reusable->sent_us = timestamp_us;
    reusable->valid = true;
  }

 private:
  struct Entry {
    uint64_t key = 0;
    uint64_t sent_us = 0;
    bool valid = false;
  };

  static bool trackable(const DiagnosticRequest& request) {
    const uint32_t can_id = request.can_id;
    return (can_id == 0x18DB33F1U || can_id == 0x18DBEFF1U ||
            can_id == 0x18DA01F1U) &&
           (request.length == 2 || request.length == 3);
  }

  static uint64_t requestKey(const DiagnosticRequest& request) {
    // 29-bit arbitration ID / 8-bit length / up to 3 read-only data bytes.
    // Other request fields such as timeout do not identify a parameter.
    return (static_cast<uint64_t>(request.can_id) << 32) |
           (static_cast<uint64_t>(request.length) << 24) |
           (static_cast<uint64_t>(request.data[0]) << 16) |
           (static_cast<uint64_t>(request.data[1]) << 8) |
           (request.length == 3 ? request.data[2] : 0U);
  }

  static uint64_t timeRemaining(uint64_t sent_us, uint64_t now_us) {
    if (now_us < sent_us) return kMinSameParameterIntervalUs;
    const uint64_t elapsed = now_us - sent_us;
    return elapsed >= kMinSameParameterIntervalUs ?
           0 : kMinSameParameterIntervalUs - elapsed;
  }

  std::array<Entry, kSlots> entries_{};
};

}  // namespace m5can
