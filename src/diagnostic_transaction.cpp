#include "diagnostic_transaction.h"

#include <cstring>

namespace m5can {

void DiagnosticTransaction::begin(const DiagnosticRequest& request,
                                  uint64_t tx_us,
                                  uint32_t cf_timeout_ms) {
  request_ = request;
  tx_us_ = tx_us;
  result_ = DiagnosticResult{};
  assembler_.begin(request, cf_timeout_ms);
}

bool DiagnosticTransaction::appendRaw(const TransactionFrame& frame) {
  if (result_.raw_frame_count >= kDiagnosticMaxRawFrames) return false;
  DiagnosticRawFrame& out = result_.raw_frames[result_.raw_frame_count++];
  out.can_id = frame.can_id;
  out.dlc = frame.dlc;
  std::memcpy(out.data, frame.data, frame.dlc);
  if (!result_.tx_to_first_response_us && frame.timestamp_us >= tx_us_) {
    result_.tx_to_first_response_us =
        static_cast<uint32_t>(frame.timestamp_us - tx_us_);
  }
  return true;
}

bool DiagnosticTransaction::appendMessage(const DiagnosticMessage& message) {
  if (result_.response_count >= kDiagnosticMaxResponses) return false;
  result_.responses[result_.response_count++] = message;
  return true;
}

TransactionStep DiagnosticTransaction::onFrame(const TransactionFrame& frame) {
  TransactionStep step{};
  if (frame.timestamp_us < tx_us_) return step;

  const IsoTpEvent event =
      assembler_.feed(frame.can_id, frame.extended, frame.rtr,
                      frame.data, frame.dlc, frame.timestamp_us);

  switch (event.type) {
    case IsoTpEventType::None:
      return step;
    case IsoTpEventType::Progress:
      appendRaw(frame);
      step.event = TransactionEvent::ResponseProgress;
      return step;
    case IsoTpEventType::Complete:
      appendRaw(frame);
      appendMessage(event.message);
      step.event = TransactionEvent::Completed;
      return step;
    case IsoTpEventType::NeedFlowControl:
      appendRaw(frame);
      step.event = TransactionEvent::NeedFlowControl;
      step.flow_control_can_id = event.flow_control_id;
      return step;
    case IsoTpEventType::ResponsePending:
      appendRaw(frame);
      step.event = TransactionEvent::ResponsePending;
      return step;
    case IsoTpEventType::Overflow:
    case IsoTpEventType::SequenceError:
    case IsoTpEventType::Collision:
    case IsoTpEventType::CapacityError:
      appendRaw(frame);
      result_.status = DiagnosticStatus::IsoTpError;
      step.event = TransactionEvent::ProtocolError;
      return step;
  }
  return step;
}

size_t DiagnosticTransaction::expireIsoTp(uint64_t now_us) {
  return assembler_.expire(now_us);
}

}  // namespace m5can
