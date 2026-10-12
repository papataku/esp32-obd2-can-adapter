#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "diagnostic_policy.h"

namespace m5can {

// CAN-owner-only cache: no lock is necessary because all reads/writes happen
// on the serialized TWAI task. Only validated, successful read-only
// transactions are reusable. 100ms is a strict maximum from completion time.
class DiagnosticResultCache {
 public:
  static constexpr uint64_t kLifetimeUs = 100000;
  static constexpr size_t kSlots = 6;

  bool lookup(const DiagnosticRequest& request, uint64_t now_us,
              DiagnosticResult& result) const {
    // Group replies are decoded and validated by the adaptive group
    // classifier; never reuse a partial response as a positive cache hit.
    if (request.length == 3 && request.data[0] == 0x01) return false;
    if (!DiagnosticPolicy::allowedReadRequest(request)) return false;
    for (const auto& e : entries_) {
      if (!e.valid || !sameKey(e.request, request)) continue;
      if (now_us < e.completed_us ||
          now_us - e.completed_us >= kLifetimeUs) return false;
      result = e.result;
      // Cached deliveries are instantaneous, not new CAN transactions.
      result.transaction_us = 0;
      result.tx_to_first_response_us = 0;
      return true;
    }
    return false;
  }

  void remember(const DiagnosticRequest& request, const DiagnosticResult& result,
                uint64_t completed_us) {
    if (request.length == 3 && request.data[0] == 0x01) return;
    if (!DiagnosticPolicy::allowedReadRequest(request)) return;
    Entry* chosen = nullptr;
    Entry* oldest = &entries_[0];
    for (auto& e : entries_) {
      if (e.valid && sameKey(e.request, request)) {
        chosen = &e;
        break;
      }
      if (!e.valid && !chosen) chosen = &e;
      if (e.completed_us < oldest->completed_us) oldest = &e;
    }
    // A failed fresh transaction invalidates any earlier successful value.
    if (!validPositiveResponse(request, result)) {
      if (chosen && chosen->valid && sameKey(chosen->request, request))
        chosen->valid = false;
      return;
    }
    if (!chosen) chosen = oldest;
    chosen->request = request;
    chosen->result = result;
    // Compatibility for callers building an older timestamp-less result.
    // Once captured here the same acquisition stamp is returned unchanged.
    chosen->result.completed_at_us =
        result.completed_at_us ? result.completed_at_us : completed_us;
    chosen->completed_us = chosen->result.completed_at_us;
    chosen->valid = true;
  }

  void clear() {
    for (auto& e : entries_) e.valid = false;
  }

 private:
  struct Entry {
    DiagnosticRequest request{};
    DiagnosticResult result{};
    uint64_t completed_us = 0;
    bool valid = false;
  };

  static bool sameKey(const DiagnosticRequest& a, const DiagnosticRequest& b) {
    return a.can_id == b.can_id && a.length == b.length &&
           a.length <= sizeof(a.data) &&
           std::memcmp(a.data, b.data, a.length) == 0;
  }

  static bool validPositiveResponse(const DiagnosticRequest& request,
                                    const DiagnosticResult& result) {
    if (result.status != DiagnosticStatus::Ok || !result.response_count ||
        !result.raw_frame_count) return false;
    for (uint8_t i = 0; i < result.response_count; ++i) {
      const auto& m = result.responses[i];
      if (m.length && m.data[0] != 0x7f &&
          DiagnosticPolicy::responseBelongsToRequest(request, m.can_id) &&
          DiagnosticPolicy::responseMatches(request, m.data, m.length))
        return true;
    }
    return false;
  }

  Entry entries_[kSlots]{};
};

}  // namespace m5can
