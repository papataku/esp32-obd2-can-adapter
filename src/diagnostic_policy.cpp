#include "diagnostic_policy.h"

namespace m5can {

bool DiagnosticPolicy::allowedHeader(uint32_t can_id) {
  // Only the physically observed ECU 01 is enabled in this phase.
  // No arbitrary 18DAxxF1 header or unrestricted diagnostic TX.
  return can_id == 0x18DB33F1U || can_id == 0x18DBEFF1U ||
         can_id == 0x18DA01F1U;
}

bool DiagnosticPolicy::allowedReadRequest(const DiagnosticRequest& request) {
  if (!allowedHeader(request.can_id)) return false;
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

  if (payload[0] == 0x7F) {
    return payload_len >= 2 && payload[1] == service;
  }
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
