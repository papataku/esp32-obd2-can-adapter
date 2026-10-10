#pragma once

#include <cstdint>

#include "diagnostic_policy.h"
#include "isotp_reassembler.h"

namespace m5can {

enum class TransactionEvent : uint8_t {
  Ignored,
  ResponseProgress,
  NeedFlowControl,
  ResponsePending,
  Completed,
  ProtocolError,
};

struct TransactionStep {
  TransactionEvent event = TransactionEvent::Ignored;
  uint32_t flow_control_can_id = 0;
};

struct TransactionFrame {
  uint32_t can_id = 0;
  bool extended = true;
  bool rtr = false;
  uint8_t dlc = 0;
  uint8_t data[8]{};
  uint64_t timestamp_us = 0;
};

class DiagnosticTransaction {
 public:
  void begin(const DiagnosticRequest& request, uint64_t tx_us,
             uint32_t cf_timeout_ms = 100);
  TransactionStep onFrame(const TransactionFrame& frame);
  size_t expireIsoTp(uint64_t now_us);
  bool hasActiveIsoTp() const { return assembler_.hasActiveSessions(); }

  const DiagnosticResult& result() const { return result_; }
  DiagnosticResult& result() { return result_; }

 private:
  bool appendRaw(const TransactionFrame& frame);
  bool appendMessage(const DiagnosticMessage& message);

  DiagnosticRequest request_{};
  uint64_t tx_us_ = 0;
  IsoTpAssembler assembler_{};
  DiagnosticResult result_{};
};

}  // namespace m5can
