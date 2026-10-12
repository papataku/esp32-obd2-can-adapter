#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "diagnostic_policy.h"

namespace m5can {

// A cache of validated *individual* diagnostic parameters. This class knows
// nothing about grouping, BLE peers, pending requests or CAN scheduling.
// One CPU1 BLE coordinator owns it; no FreeRTOS lock or heap allocation.
// Raw CAN capture is independent: virtual ELM frames may be stored for a
// previously validated grouped response but are NEVER RAW capture evidence.
class DiagnosticPidCache {
 public:
  static constexpr size_t kSlots = 12;
  static constexpr size_t kMaxCachedRawFrames = 12;
  static constexpr uint64_t kDefaultMaxAgeUs = 100000ULL;

  struct Stats {
    uint32_t hits = 0;
    uint32_t misses = 0;
    uint32_t inserts = 0;
    uint32_t invalidations = 0;
    uint32_t evictions = 0;
  };

  Stats stats() const { return stats_; }

  bool hasFresh(const DiagnosticRequest& request, uint64_t now_us,
                uint64_t max_age_us = kDefaultMaxAgeUs) const {
    const Entry* e = find(request);
    return e && fresh(*e, now_us, max_age_us);
  }

  bool lookup(const DiagnosticRequest& request, uint64_t now_us,
              DiagnosticResult& result,
              uint64_t max_age_us = kDefaultMaxAgeUs) {
    const Entry* e = find(request);
    if (!e || !fresh(*e, now_us, max_age_us)) {
      ++stats_.misses;
      return false;
    }
    // An aggregate temporary would place ~1.7 KiB on the calling BLE
    // task stack when optimized/inlined. This POD workspace is owned by
    // the coordinator; zero it in place without an aggregate temporary.
    std::memset(static_cast<void*>(&result),0,sizeof(result));
    result.status = DiagnosticStatus::Ok;
    result.response_count = 1;
    result.responses[0] = e->message;
    result.raw_frame_count = e->raw_count;
    for (uint8_t i = 0; i < e->raw_count; ++i)
      result.raw_frames[i] = e->raw[i];
    // This is not an additional physical CAN transaction.
    result.transaction_us = 0;
    result.tx_to_first_response_us = 0;
    ++stats_.hits;
    return true;
  }

  // Only cache a single unambiguous positive service 01 PID response.
  // Other services (including UDS variable-length DIDs) stay on the
  // existing conservative request-cache path until individually proven.
  bool rememberSingle(const DiagnosticRequest& request,
                      const DiagnosticResult& result,
                      uint64_t completed_us) {
    if (!trackable(request) || result.status != DiagnosticStatus::Ok ||
        result.response_count != 1 || result.raw_frame_count != 1) {
      invalidate(request);
      return false;
    }
    const auto& message = result.responses[0];
    if (!validMessage(request, message)) {
      invalidate(request);
      return false;
    }
    // The cached reply must correspond exclusively to this ECU.
    // All admitted Mode01 PIDs fit in one Classical CAN Single Frame.
    // Validate the REAL SF payload against the decoded message, not just
    // its arbitration ID; never cache a parser/transport-inconsistent value.
    const auto& frame=result.raw_frames[0];
    if (frame.can_id!=message.can_id ||
        frame.dlc<message.length+1U ||
        frame.data[0]!=message.length ||
        std::memcmp(&frame.data[1],message.data,message.length)!=0) {
      invalidate(request);
      return false;
    }
    return put(request, message, result.raw_frames,
               result.raw_frame_count, completed_us);
  }

  // Called only AFTER the independent grouping policy has approved a complete
  // single-ECU response. Store two separate PID records. In case of an
  // impossible malformed virtual response, insert NEITHER cache entry.
  bool rememberApprovedPair(const DiagnosticRequest& rpm,
                            const DiagnosticRequest& speed,
                            const DiagnosticRawFrame frames[2],
                            uint64_t completed_us) {
    if (!trackable(rpm) || !trackable(speed) ||
        rpm.can_id != speed.can_id ||
        rpm.data[1] != 0x0C || speed.data[1] != 0x0D)
      return false;
    DiagnosticMessage messages[2]{};
    for (unsigned i = 0; i < 2; ++i) {
      const auto& f = frames[i];
      if (f.dlc != 8 || f.data[0] > 7 || f.data[0] < 3 ||
          f.data[1] != 0x41 ||
          f.data[2] != (i == 0 ? 0x0C : 0x0D))
        return false;
      messages[i].can_id = f.can_id;
      messages[i].length = f.data[0];
      std::memcpy(messages[i].data, &f.data[1], f.data[0]);
    }
    if (messages[0].can_id != messages[1].can_id ||
        !validMessage(rpm, messages[0]) ||
        !validMessage(speed, messages[1]))
      return false;
    // Ensure atomic semantics across the pair: never write RPM and then
    // discover that speed belongs to a different previously learned ECU.
    const Entry* old_rpm=find(rpm);
    const Entry* old_speed=find(speed);
    const uint32_t ecu=messages[0].can_id;
    if ((old_rpm && old_rpm->message.can_id!=ecu) ||
        (old_speed && old_speed->message.can_id!=ecu)) {
      invalidate(rpm);
      invalidate(speed);
      return false;
    }
    return put(rpm, messages[0], &frames[0], 1, completed_us) &&
           put(speed, messages[1], &frames[1], 1, completed_us);
  }

  void invalidate(const DiagnosticRequest& request) {
    for (auto& e : entries_) {
      if (e.valid && sameKey(e.request, request)) {
        e.valid = false;
        ++stats_.invalidations;
      }
    }
  }
  void clear() {
    for (auto& e : entries_) e.valid = false;
  }

 private:
  struct Entry {
    DiagnosticRequest request{};
    DiagnosticMessage message{};
    DiagnosticRawFrame raw[kMaxCachedRawFrames]{};
    uint64_t completed_us = 0;
    uint8_t raw_count = 0;
    bool valid = false;
  };

  static bool trackable(const DiagnosticRequest& request) {
    // Intentional: only the Mode 01 PID set, not a compound CAN payload.
    // Require an established fixed PID payload format. A service-01
    // allowlist alone does NOT establish the response length of unknown PIDs.
    // Mode01 0x9A and variable manufacturer data remain uncached until
    // their precise length/ECU identity is independently established.
    const uint8_t pid=request.data[1];
    const bool known=(pid==0x0C || pid==0x0D ||
                      pid==0x05 || pid==0x5B);
    return known && request.can_id == 0x18DB33F1U &&
           request.length == 2 && request.data[0] == 0x01 &&
           DiagnosticPolicy::allowedReadRequest(request);
  }
  static bool sameKey(const DiagnosticRequest& lhs,
                      const DiagnosticRequest& rhs) {
    return lhs.can_id == rhs.can_id && lhs.length == rhs.length &&
           lhs.data[0] == rhs.data[0] && lhs.data[1] == rhs.data[1];
  }
  static bool fresh(const Entry& e, uint64_t now, uint64_t max_age) {
    return e.valid && now >= e.completed_us &&
           now - e.completed_us < max_age &&
           max_age <= kDefaultMaxAgeUs;
  }
  const Entry* find(const DiagnosticRequest& request) const {
    if (!trackable(request)) return nullptr;
    for (const auto& e : entries_)
      if (e.valid && sameKey(e.request, request)) return &e;
    return nullptr;
  }
  static bool validMessage(const DiagnosticRequest& request,
                           const DiagnosticMessage& message) {
    // For the 4 known meter PIDs the payload length is fixed. Never
    // cache a truncated RPM (missing its second byte) as a fresh sample.
    const uint16_t exact_length=request.data[1]==0x0C?4U:3U;
    return message.length == exact_length &&
           message.data[0] == 0x41 &&
           message.data[1] == request.data[1] &&
           DiagnosticPolicy::responseBelongsToRequest(request,message.can_id) &&
           DiagnosticPolicy::responseMatches(
               request,message.data,message.length);
  }
  bool put(const DiagnosticRequest& request, const DiagnosticMessage& message,
           const DiagnosticRawFrame* frames, uint8_t n,
           uint64_t completed_us) {
    if (!trackable(request) || !validMessage(request,message) ||
        n == 0 || n > kMaxCachedRawFrames)
      return false;
    Entry* chosen = nullptr;
    Entry* oldest = &entries_[0];
    for (auto& e : entries_) {
      if (e.valid && sameKey(e.request,request)) {
        // A functional address must not silently switch responders.
        // Invalidate on unexpected ECU provenance, rather than treating
        // a different ECU's value as the same physical signal.
        if (e.message.can_id != message.can_id) {
          e.valid=false;
          ++stats_.invalidations;
          return false;
        }
        chosen = &e;
        break;
      }
      if (!chosen && !e.valid) chosen = &e;
      if (e.completed_us < oldest->completed_us) oldest = &e;
    }
    if (!chosen) {
      chosen = oldest;
      ++stats_.evictions;
    }
    chosen->request = request;
    chosen->message = message;
    chosen->completed_us = completed_us;
    chosen->raw_count = n;
    for (uint8_t i = 0; i < n; ++i)
      chosen->raw[i] = frames[i];
    chosen->valid = true;
    ++stats_.inserts;
    return true;
  }
  std::array<Entry,kSlots> entries_{};
  Stats stats_{};
};

}  // namespace m5can
