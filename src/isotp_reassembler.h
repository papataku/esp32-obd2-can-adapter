#pragma once

#include <Arduino.h>

#include "diagnostic_policy.h"

namespace m5can {

enum class IsoTpEventType : uint8_t {
  None,
  Progress,
  Complete,
  NeedFlowControl,
  Overflow,
  ResponsePending,
  SequenceError,
  Collision,
  CapacityError,
};

struct IsoTpEvent {
  IsoTpEventType type = IsoTpEventType::None;
  uint32_t response_id = 0;
  uint32_t flow_control_id = 0;
  DiagnosticMessage message{};
};

class IsoTpAssembler {
 public:
  void begin(const DiagnosticRequest& request, uint32_t cf_timeout_ms = 100);
  IsoTpEvent feed(uint32_t can_id, bool extended, bool rtr,
                  const uint8_t* data, uint8_t dlc, uint64_t timestamp_us);
  uint8_t expire(uint64_t now_us);
  bool hasActiveSessions() const;
  void abort(uint32_t response_id);

  static uint32_t flowControlIdForResponse(uint32_t response_id);

 private:
  struct Session {
    bool active = false;
    uint32_t response_id = 0;
    uint16_t expected_length = 0;
    uint16_t received = 0;
    uint8_t next_sequence = 1;
    uint64_t deadline_us = 0;
    DiagnosticMessage message{};
  };

  Session* find(uint32_t response_id);
  Session* allocate(uint32_t response_id);
  bool initialPayloadMatches(const uint8_t* payload, size_t length) const;
  // V2 0C/0D physical probe or essential read: only Single Frames allowed.
  bool physicalProbeRoute(uint32_t can_id) const {
    return request_.signal_v2 && request_.length==3 &&
           request_.can_id!=0x18DB33F1U &&
           DiagnosticPolicy::responseBelongsToRequest(request_,can_id);
  }

  DiagnosticRequest request_{};
  uint32_t cf_timeout_ms_ = 100;
  Session sessions_[kDiagnosticMaxResponses]{};
};

}  // namespace m5can
