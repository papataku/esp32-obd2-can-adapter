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
  bool signal_v2 = false; // Dedicated V2 internal CAN owner.
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
  // One monotonic timestamp for the ACTUAL CAN acquisition. A cache hit
  // must preserve this time, not extend freshness on each delivery.
  uint64_t completed_at_us = 0;
  uint32_t tx_to_first_response_us = 0;
  uint32_t transaction_us = 0;
  uint8_t response_count = 0;
  DiagnosticMessage responses[kDiagnosticMaxResponses]{};
  uint8_t raw_frame_count = 0;
  DiagnosticRawFrame raw_frames[kDiagnosticMaxRawFrames]{};
  // DiagnosticResult is transient, never persisted as the legacy trip ABI.
  // Physical accepted CAN-frame RX times relative to request TX.
  uint32_t raw_rx_offsets_us[kDiagnosticMaxRawFrames]{};
};

class DiagnosticPolicy {
 public:
  static bool allowedHeader(uint32_t can_id);
  static bool observedPhysicalEcu(uint32_t request_id);
  static void observeFunctionalPairs(const DiagnosticRequest& request,
                                     const DiagnosticResult& result);
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
