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
    if (grouped(request)) {
      // Preserve the established 50ms PER-PID guard even via grouped TX.
      const DiagnosticRequest rpm = singleFromGroup(request, 0x0C);
      const DiagnosticRequest speed = singleFromGroup(request, 0x0D);
      const uint64_t a = remainingUs(rpm, now_us);
      const uint64_t b = remainingUs(speed, now_us);
      if (a || b) return a > b ? a : b;
      // Individual eligibility is insufficient when 31 of 32 slots are
      // occupied: each new constituent PID needs its OWN throttle slot.
      // Otherwise the second noteSuccessfulTx silently loses its history.
      const uint64_t rpm_key=requestKey(rpm);
      const uint64_t speed_key=requestKey(speed);
      size_t free_slots=0;
      bool rpm_seen=false,speed_seen=false;
      uint64_t earliest=kMinSameParameterIntervalUs;
      for (const Entry& entry:entries_) {
        if (entry.valid && entry.key==rpm_key) rpm_seen=true;
        if (entry.valid && entry.key==speed_key) speed_seen=true;
        if (!entry.valid || timeRemaining(entry.sent_us,now_us)==0)
          ++free_slots;
        else {
          const uint64_t remaining=timeRemaining(entry.sent_us,now_us);
          if (remaining<earliest) earliest=remaining;
        }
      }
      const size_t needed=static_cast<size_t>(!rpm_seen)+
                          static_cast<size_t>(!speed_seen);
      return free_slots>=needed ? 0 : earliest;
    }
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
    if (grouped(request)) {
      noteSuccessfulTx(singleFromGroup(request, 0x0C), timestamp_us);
      noteSuccessfulTx(singleFromGroup(request, 0x0D), timestamp_us);
      return;
    }
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

  static bool grouped(const DiagnosticRequest& request) {
    return request.can_id == 0x18DB33F1U && request.length == 3 &&
           request.data[0] == 0x01 && request.data[1] == 0x0C &&
           request.data[2] == 0x0D;
  }

  static DiagnosticRequest singleFromGroup(const DiagnosticRequest& group,
                                           uint8_t pid) {
    DiagnosticRequest item = group;
    item.length = 2;
    item.data[1] = pid;
    item.data[2] = 0;
    return item;
  }

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
