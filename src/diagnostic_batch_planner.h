#pragma once

#include <cstdint>
#include <cstring>

#include "adaptive_multi_pid.h"
#include "diagnostic_policy.h"

namespace m5can {

// Pure, allocation-free physical CAN transmit planner.
// INPUTS MUST already be due and not individually fulfilled by the cache.
// No cache, BLE, peer, queue, timeout scheduling or IO dependencies.
// This planner NEVER invents an unrequested PID.
class DiagnosticBatchPlanner {
 public:
  enum class Kind : uint8_t { Single, JoinSameParameter, GroupDistinct };
  struct Plan {
    Kind kind = Kind::Single;
    DiagnosticRequest transmission{};
  };

  static Plan plan(const DiagnosticRequest& current,
                   const DiagnosticRequest* other_pending,
                   const AdaptiveMultiPid& capability) {
    Plan output{};
    output.transmission=current;
    if (!other_pending) return output;
    const auto& other=*other_pending;
    // Join matching pending intents even when the ECU has no grouping
    // support. Timeout semantics must agree: no late reply for a caller
    // whose timeout was shorter than the physical CAN transaction.
    if (sameRequest(current,other)) {
      output.kind=Kind::JoinSameParameter;
      return output;
    }
    const DiagnosticRequest *rpm=nullptr,*speed=nullptr;
    if (AdaptiveMultiPid::isPair(current,other)) {
      rpm=&current;speed=&other;
    } else if (AdaptiveMultiPid::isPair(other,current)) {
      rpm=&other;speed=&current;
    }
    if (!rpm || rpm->timeout_ms!=speed->timeout_ms ||
        !capability.eligible(*rpm,*speed)) return output;
    const DiagnosticRequest group=AdaptiveMultiPid::group(*rpm,*speed);
    if (!DiagnosticPolicy::allowedReadRequest(group)) return output;
    output.kind=Kind::GroupDistinct;
    output.transmission=group;
    return output;
  }

  static bool sameRequest(const DiagnosticRequest& a,
                          const DiagnosticRequest& b) {
    return a.length>0 && a.length<=sizeof(a.data) &&
           b.length==a.length && a.can_id==b.can_id &&
           a.timeout_ms==b.timeout_ms &&
           std::memcmp(a.data,b.data,a.length)==0;
  }
};

}  // namespace m5can
