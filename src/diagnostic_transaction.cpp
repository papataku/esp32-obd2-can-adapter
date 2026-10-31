#include "diagnostic_transaction.h"

#include <cstring>

namespace m5can {

void DiagnosticTransaction::begin(const DiagnosticRequest& request,
                                  uint64_t tx_us,
                                  uint32_t cf_timeout_ms) {
  failed_ = false;
  rx_count_ = fc_count_ = seen_pids_ = 0;
  request_ = request;
  tx_us_ = tx_us;
  result_ = DiagnosticResult{};
  assembler_.begin(request, cf_timeout_ms);
}

bool DiagnosticTransaction::appendRaw(const TransactionFrame& frame) {
  if (result_.raw_frame_count >= kDiagnosticMaxRawFrames) return false;
  const uint8_t index=result_.raw_frame_count++;
  DiagnosticRawFrame& out = result_.raw_frames[index];
  out.can_id = frame.can_id;
  out.dlc = frame.dlc;
  result_.raw_rx_offsets_us[index] = frame.timestamp_us >= tx_us_ &&
      frame.timestamp_us-tx_us_ < UINT32_MAX
      ? static_cast<uint32_t>(frame.timestamp_us-tx_us_) : UINT32_MAX;
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
  if (failed_) { step.event = TransactionEvent::ProtocolError; return step; }

  auto fail = [&]() {
    failed_ = true;
    result_.status = DiagnosticStatus::IsoTpError;
    result_.response_count = 0;
    result_.completed_at_us = 0;
    step.event = TransactionEvent::ProtocolError;
    return step;
  };
  const bool probe=request_.signal_v2 && request_.length==3 &&
                   request_.can_id!=0x18DB33F1U;
  const bool diagnostic=DiagnosticPolicy::allowedResponseId(frame.can_id) ||
      (!frame.extended && frame.can_id>=0x7E8 && frame.can_id<=0x7EF);
  // A V2 physical transaction cannot silently absorb a different diagnostic
  // route and then publish the pinned source as healthy. Refuse before FC.
  if (request_.signal_v2 && diagnostic && frame.timestamp_us >= tx_us_ &&
      (!frame.extended || frame.rtr || frame.dlc>8 ||
       !DiagnosticPolicy::responseBelongsToRequest(request_,frame.can_id)))
    return fail();
  if (probe && diagnostic) {
    // Validate before the assembler can silently filter a contract breach.
    if (frame.timestamp_us < tx_us_ || frame.dlc>8 || !frame.extended ||
        frame.rtr || !DiagnosticPolicy::responseBelongsToRequest(request_,frame.can_id) ||
        frame.dlc<2 || frame.data[0]==0 || frame.data[0]>7 ||
        frame.data[0]+1U>frame.dlc ||
        !DiagnosticPolicy::responseMatches(request_,frame.data+1,frame.data[0]))
      return fail();
  }
  if (frame.timestamp_us < tx_us_ || frame.dlc>8) return step;
  // Count every candidate route frame, including malformed/duplicate PDUs.
  // Storage capacity is never permission for more induced bus traffic.
  if (request_.signal_v2 && frame.extended && !frame.rtr &&
      DiagnosticPolicy::responseBelongsToRequest(request_, frame.can_id)) {
    if (frame.timestamp_us > tx_us_ + uint64_t(request_.timeout_ms)*1000 ||
        ++rx_count_ > (request_.length==3 && request_.can_id!=0x18DB33F1U?2:10)) return fail();
  }

  const IsoTpEvent event =
      assembler_.feed(frame.can_id, frame.extended, frame.rtr,
                      frame.data, frame.dlc, frame.timestamp_us);

  if (event.type != IsoTpEventType::None) {
    if (!appendRaw(frame)) return fail();
    if (request_.signal_v2 && event.type == IsoTpEventType::NeedFlowControl &&
        (++fc_count_ > 1 || seen_pids_ == 31 ||
         (frame.dlc>=4 && ((frame.data[1]==17 && seen_pids_) ||
                           (frame.data[3]==0x9A && (seen_pids_ & 16)))))) return fail();
    if (request_.signal_v2 && event.type == IsoTpEventType::Complete) {
      // Physical replies may be a group or disjoint per-PID PDUs only.
      // Functional discovery has separate responders, checked below.
      if (request_.can_id != 0x18DB33F1U) {
        const auto &m = event.message;
        const unsigned max_pdus=request_.length==3?2:5;
        if(result_.response_count>=max_pdus)return fail();
        for(unsigned i=0;i<result_.response_count;++i){
          const auto &prior=result_.responses[i];
          if(m.data[0]!=0x41 || prior.data[0]!=0x41 ||
             (prior.length==m.length && !std::memcmp(prior.data,m.data,m.length)))return fail();
        }
        uint8_t mask = 0;
        size_t pos = 1;
        while (m.length > 1 && m.data[0] == 0x41 && pos < m.length) {
          const uint8_t pids[] = {0x0D,0x0C,0x05,0x5B,0x9A};
          const uint8_t sizes[] = {2,3,2,2,7};
          unsigned j=0;
          for (; j<5 && m.data[pos]!=pids[j]; ++j) {}
          if (j==5 || (probe && j>1) || pos+sizes[j]>m.length || (mask & (1U<<j))) return fail();
          mask |= 1U<<j; pos += sizes[j];
        }
        if (mask & seen_pids_) return fail();
        seen_pids_ |= mask;
      } else {
        for (unsigned i=0;i<result_.response_count;++i)
          if (result_.responses[i].can_id==event.message.can_id) return fail();
      }
    }
  }
  switch (event.type) {
    case IsoTpEventType::None:
      return step;
    case IsoTpEventType::Progress:
      step.event = TransactionEvent::ResponseProgress;
      return step;
    case IsoTpEventType::Complete:
      if (!appendMessage(event.message)) return fail();
      if (!result_.completed_at_us) {
        // Earliest completed response: do not grant extra freshness for
        // the later quiet window, mode switching or result delivery.
        result_.completed_at_us=frame.timestamp_us;
      }
      step.event = TransactionEvent::Completed;
      return step;
    case IsoTpEventType::NeedFlowControl:
      step.event = TransactionEvent::NeedFlowControl;
      step.flow_control_can_id = event.flow_control_id;
      return step;
    case IsoTpEventType::ResponsePending:
      step.event = TransactionEvent::ResponsePending;
      return step;
    case IsoTpEventType::Overflow:
    case IsoTpEventType::SequenceError:
    case IsoTpEventType::Collision:
    case IsoTpEventType::CapacityError:
      return fail();
  }
  return step;
}

size_t DiagnosticTransaction::expireIsoTp(uint64_t now_us) {
  return assembler_.expire(now_us);
}

}  // namespace m5can
