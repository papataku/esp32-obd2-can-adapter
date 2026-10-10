#pragma once

#include <Arduino.h>

namespace m5can {

constexpr size_t kDiagnosticMaxPayload = 96;
constexpr size_t kDiagnosticMaxResponses = 8;
constexpr size_t kDiagnosticMaxRawFrames = 48;

enum class DiagnosticStatus : uint8_t {
  Ok,
  NoData,
  Busy,
  LeaseRequired,
  InvalidRequest,
  RateLimited,
  DriverError,
  BusOff,
  ModeError,
  IsoTpError,
};

struct DiagnosticRequest {
  uint32_t can_id = 0;
  uint8_t length = 0;
  uint8_t data[8]{};
  uint32_t timeout_ms = 200;
};

struct DiagnosticRawFrame {
  uint32_t can_id = 0;
  uint8_t dlc = 0;
  uint8_t data[8]{};
};

struct DiagnosticMessage {
  uint32_t can_id = 0;
  uint16_t length = 0;
  uint8_t data[kDiagnosticMaxPayload]{};
};

struct DiagnosticResult {
  DiagnosticStatus status = DiagnosticStatus::NoData;
  uint32_t tx_to_first_response_us = 0;
  uint32_t transaction_us = 0;
  uint8_t response_count = 0;
  DiagnosticMessage responses[kDiagnosticMaxResponses]{};
  uint8_t raw_frame_count = 0;
  DiagnosticRawFrame raw_frames[kDiagnosticMaxRawFrames]{};
};

class DiagnosticPolicy {
 public:
  static bool allowedHeader(uint32_t can_id);
  static bool allowedReadRequest(const DiagnosticRequest& request);
  static bool allowedResponseId(uint32_t can_id);
  static bool responseBelongsToRequest(const DiagnosticRequest& request,
                                       uint32_t response_can_id);
  static bool responseMatches(const DiagnosticRequest& request,
                              const uint8_t* payload, size_t payload_len);
  static uint32_t transactionBudgetMs(uint32_t response_timeout_ms,
                                      bool allow_response_pending = false);
};

}  // namespace m5can
