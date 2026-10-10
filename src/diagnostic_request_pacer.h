#pragma once

#include <cstdint>

namespace m5can {

// The 50-ms diagnostic spacing applies to repeated requests using the SAME
// CAN arbitration ID. Distinct explicitly allowed diagnostic request IDs
// have independent clocks; the CAN owner still serializes transactions.
// Flow-control frames are not diagnostic requests and must not be delayed.
class DiagnosticRequestPacer {
 public:
  static constexpr uint64_t kMinSameIdIntervalUs = 50000ULL;

  uint64_t remainingUs(uint32_t can_id, uint64_t now_us) const {
    const int index = slotFor(can_id);
    if (index < 0 || !valid_[index]) return 0;
    const uint64_t previous = last_sent_us_[index];
    if (now_us < previous) return kMinSameIdIntervalUs;
    const uint64_t elapsed = now_us - previous;
    return elapsed < kMinSameIdIntervalUs
               ? kMinSameIdIntervalUs - elapsed
               : 0;
  }

  void noteSuccessfulTx(uint32_t can_id, uint64_t timestamp_us) {
    const int index = slotFor(can_id);
    if (index < 0) return;
    last_sent_us_[index] = timestamp_us;
    valid_[index] = true;
  }

 private:
  // These are the *only* whitelisted diagnostic arbitration IDs.
  // Do not increase this list without explicit safety review.
  static int slotFor(uint32_t can_id) {
    switch (can_id) {
      case 0x18DB33F1U: return 0;  // OBD functional
      case 0x18DBEFF1U: return 1;  // UDS functional
      case 0x18DA01F1U: return 2;  // ECU 01 physical UDS
      default: return -1;          // unauthorized by DiagnosticPolicy
    }
  }

  uint64_t last_sent_us_[3]{};
  bool valid_[3]{};
};

}  // namespace m5can
