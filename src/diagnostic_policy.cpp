#include "diagnostic_policy.h"
#include <atomic>
#include <cstring>

namespace m5can {

namespace {
// RAM-only provenance. No boot-time physical Mode01 authorization.
#if defined(M5CAN_ECU_SELECTION_EXPERIMENTAL)
std::atomic<uint32_t> seen_ecu[8]{};
bool isPhysical(uint32_t id) {
  return (id & 0x1FFF00FFU)==0x18DA00F1U;
}
#endif
}
bool DiagnosticPolicy::observedPhysicalEcu(uint32_t id) {
#if defined(M5CAN_ECU_SELECTION_EXPERIMENTAL)
  if(!isPhysical(id))return false;
  const uint8_t source=static_cast<uint8_t>(id>>8);
  return (seen_ecu[source/32].load(std::memory_order_acquire) &
          (1UL << (source%32)))!=0;
#else
  (void)id;return false;
#endif
}
void DiagnosticPolicy::observeFunctionalPairs(const DiagnosticRequest& req,
                                              const DiagnosticResult& r) {
#if defined(M5CAN_ECU_SELECTION_EXPERIMENTAL)
  if(req.can_id!=0x18DB33F1U || req.length!=3 || req.data[0]!=0x01 ||
     req.data[1]!=0x0C || req.data[2]!=0x0D ||
     r.status!=DiagnosticStatus::Ok || r.response_count>kDiagnosticMaxResponses)return;
  for(uint8_t i=0;i<r.response_count;++i) {
    const auto& m=r.responses[i];
    if((m.can_id&0x1FFFFF00U)!=0x18DAF100U || m.length!=6 ||
       m.data[0]!=0x41 || !((m.data[1]==0x0C && m.data[4]==0x0D) ||
                             (m.data[1]==0x0D && m.data[3]==0x0C)))continue;
    bool dup=false;
    for(uint8_t k=0;k<r.response_count;++k)
      if(k!=i && r.responses[k].can_id==m.can_id)dup=true;
    if(dup)continue;
    bool backed=false;
    for(uint8_t j=0;j<r.raw_frame_count && j<kDiagnosticMaxRawFrames;++j) {
      const auto& f=r.raw_frames[j];
      if(f.can_id==m.can_id && f.dlc>=7 && f.data[0]==6 &&
         std::memcmp(&f.data[1],m.data,6)==0)backed=true;
    }
    if(!backed)continue;
    const uint8_t source=static_cast<uint8_t>(m.can_id);
    seen_ecu[source/32].fetch_or(1UL<<(source%32),std::memory_order_release);
  }
#else
  (void)req;(void)r;
#endif
}

bool DiagnosticPolicy::allowedHeader(uint32_t can_id) {
  // Only the physically observed ECU 01 is enabled in this phase.
  // No arbitrary 18DAxxF1 header or unrestricted diagnostic TX.
  return can_id == 0x18DB33F1U || can_id == 0x18DBEFF1U ||
         can_id == 0x18DA01F1U || observedPhysicalEcu(can_id);
}

bool DiagnosticPolicy::allowedReadRequest(const DiagnosticRequest& request) {
  if (!allowedHeader(request.can_id)) return false;
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
  if (!request.signal_v2) return false; // V1/USB/legacy TX never permitted.
  if (request.can_id==0x18DB33F1U)
    return request.length==3 && request.data[0]==1 &&
           request.data[1]==0x0C && request.data[2]==0x0D;
  if(!observedPhysicalEcu(request.can_id)||request.data[0]!=1)
    return false;
  const bool full=request.length==6 && request.data[1]==0x0D &&
      request.data[2]==0x0C && request.data[3]==0x05 &&
      request.data[4]==0x5B && request.data[5]==0x9A;
  const bool essential=request.length==3 && request.data[1]==0x0C &&
      request.data[2]==0x0D;
  return full||essential;
#endif
#if defined(M5CAN_ECU_SELECTION_EXPERIMENTAL)
  if(observedPhysicalEcu(request.can_id)) {
    if(request.data[0]==0x01 && request.length==2)
      return request.data[1]==0x0C || request.data[1]==0x0D;
    if(request.data[0]==0x01 && request.length==3 &&
       request.data[1]==0x0C && request.data[2]==0x0D)return true;
    if(request.can_id!=0x18DA01F1U)return false;
  }
#endif
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
  // Opt-in, exact read-only two-PID pilot. No arbitrary Mode01 payloads.
  if (request.can_id == 0x18DB33F1U &&
      request.length == 3 && request.data[0] == 0x01 &&
      request.data[1] == 0x0C && request.data[2] == 0x0D) return true;
#endif
  if (request.can_id == 0x18DA01F1U) {
    // The narrowly-scoped physical address is UDS 0x22 read only.
    return request.length == 3 && request.data[0] == 0x22;
  }
  if (request.length == 2 &&
      (request.data[0] == 0x01 || request.data[0] == 0x09)) {
    return true;
  }
  return request.length == 3 && request.data[0] == 0x22;
}

bool DiagnosticPolicy::allowedResponseId(uint32_t can_id) {
  return (can_id & 0x1FFFFF00U) == 0x18DAF100U;
}

bool DiagnosticPolicy::responseBelongsToRequest(
    const DiagnosticRequest& request, uint32_t response_can_id) {
  if (!allowedResponseId(response_can_id)) return false;
  if (observedPhysicalEcu(request.can_id) &&
      request.data[0]==0x01 && allowedReadRequest(request))
    return response_can_id ==
      (0x18DAF100U | ((request.can_id>>8)&0xFFU));
  if (request.can_id == 0x18DA01F1U) {
    // Never accept another ECU's response or send it Flow Control.
    return response_can_id == 0x18DAF101U;
  }
  return allowedHeader(request.can_id);
}

bool DiagnosticPolicy::responseMatches(const DiagnosticRequest& request,
                                       const uint8_t* payload,
                                       size_t payload_len) {
  if (!payload || !payload_len || !allowedReadRequest(request)) return false;
  const uint8_t service = request.data[0];
  // A negative response is valid transport evidence, but never a meter value.
  // Accepting its NRC is required to distinguish NO DATA and ISO-TP error.
  if (payload[0] == 0x7F)
    return payload_len >= 2 && payload[1] == service;
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
  if(request.signal_v2 && request.length==6) {
    if(payload_len<2 || payload[0]!=0x41)return false;
    const uint8_t pid=payload[1];
    return pid==0x0D||pid==0x0C||pid==0x05||
           pid==0x5B||pid==0x9A;
  }
#endif

#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
  if (service == 0x01 && request.length == 3 &&
      (request.can_id == 0x18DB33F1U || observedPhysicalEcu(request.can_id)) &&
      request.data[1] == 0x0C && request.data[2] == 0x0D) {
    // For a multi-PID reply, validate full contents only AFTER reassembly.
    // This filter accepts an initial ISO-TP frame only when it belongs to a
    // requested PID. An unrelated tester's FF must never receive our FC.
    return payload_len >= 2 && payload[0] == 0x41 &&
           (payload[1] == 0x0C || payload[1] == 0x0D);
  }
#endif
  if (service == 0x01 || service == 0x09) {
    return payload_len >= 2 &&
           payload[0] == static_cast<uint8_t>(service + 0x40) &&
           payload[1] == request.data[1];
  }
  if (service == 0x22) {
    return payload_len >= 3 && payload[0] == 0x62 &&
           payload[1] == request.data[1] &&
           payload[2] == request.data[2];
  }
  return false;
}

uint32_t DiagnosticPolicy::transactionBudgetMs(uint32_t response_timeout_ms,
                                               bool allow_response_pending) {
  constexpr uint32_t kModeTransitionAndSafetyMarginMs = 150;
  const uint32_t bounded =
      response_timeout_ms > 1000 ? 1000 : response_timeout_ms;
  const uint32_t response_window =
      allow_response_pending && bounded < 1000 ? 1000 : bounded;
  return response_window + kModeTransitionAndSafetyMarginMs;
}

}  // namespace m5can
