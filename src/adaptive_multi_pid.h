#pragma once

#include <cstdint>
#include <cstring>
#include "diagnostic_policy.h"

namespace m5can {

// Experimental single-frame Mode 01 pilot. A capability decision is shared by
// both BLE sessions and lasts until firmware reboot. Never auto-probe repeatedly
// after an unsupported, timed-out or malformed response.
class AdaptiveMultiPid {
 public:
  enum class Support : uint8_t { Unknown, Supported, Disabled };
  enum class Decision : uint8_t { UseGrouped, RetryLegacy, Stop };
  // Stable numeric codes for bounded event-time USB diagnostics.
  enum class RejectReason : uint8_t {
    None=0, Transport=1, NoResponse=2, InvalidPair=3,
    DuplicateEcu=4, ValuesDisagree=5, LearnedEcuMissing=6
  };
  RejectReason lastRejectReason() const { return last_reject_reason_; }

  Support support() const { return support_; }
  // Inspect provenance even after the profile has been disabled. A reply
  // from a different functional ECU must never fill this profile's cache.
  bool responderMatchesLearnedEcu(uint32_t can_id) const {
    return responder_can_id_ == 0 || can_id == responder_can_id_;
  }
  void markSupported(uint32_t responder) {
    support_ = Support::Supported;
    responder_can_id_ = responder;
  }
  void disable() { support_ = Support::Disabled; }

  // One classification path for BOTH BLE clients. Transport/bus/lease
  // errors must not start extra diagnostic TX. Supported groups must
  // decode ALL values from one responder before being exposed to meters.
  Decision evaluate(const DiagnosticResult& result,
                    DiagnosticRawFrame frames[2], uint32_t& responder) {
    RejectReason reason=RejectReason::None;
    if (decode(result, frames, responder, &reason)) {
      last_reject_reason_=RejectReason::None;
      markSupported(responder);
      return Decision::UseGrouped;
    }
    last_reject_reason_=reason;
    disable();
    switch (result.status) {
      case DiagnosticStatus::Ok:      // negative or incomplete ECU reply
      case DiagnosticStatus::NoData:  // timeout / unsupported
      case DiagnosticStatus::IsoTpError:
        return Decision::RetryLegacy;
      default:                       // lease, bus, driver, policy, busy
        return Decision::Stop;
    }
  }

  static bool isPair(const DiagnosticRequest& rpm,
                     const DiagnosticRequest& speed) {
    return rpm.can_id == 0x18DB33F1U &&
           speed.can_id == rpm.can_id &&
           rpm.length == 2 && speed.length == 2 &&
           rpm.data[0] == 0x01 && rpm.data[1] == 0x0C &&
           speed.data[0] == 0x01 && speed.data[1] == 0x0D;
  }

  bool eligible(const DiagnosticRequest& rpm,
                const DiagnosticRequest& speed) const {
    return support_ != Support::Disabled && isPair(rpm, speed);
  }

  static DiagnosticRequest group(const DiagnosticRequest& rpm,
                                 const DiagnosticRequest& speed) {
    DiagnosticRequest grouped = rpm;
    grouped.length = 3;
    grouped.data[2] = speed.data[1];
    // Both client items have one common transaction timeout.
    if (speed.timeout_ms > grouped.timeout_ms)
      grouped.timeout_ms = speed.timeout_ms;
    return grouped;
  }

  // CAN owner statistics measure physical positive response reception,
  // not the CPU1 coordinator's group acceptance. A valid complete pair
  // from ANY responder counts as transport-positive even when an unlearned
  // group later falls back due to conflicting ECU values. The actual
  // grouped-success count is maintained by the BLE coordinator.
  static bool hasCompletePositivePair(const DiagnosticResult& result,
                                      uint32_t& positive_ecu) {
    positive_ecu=0;
    if (result.status!=DiagnosticStatus::Ok ||
        result.response_count==0 ||
        result.response_count>kDiagnosticMaxResponses) return false;
    for(uint8_t i=0;i<result.response_count;++i) {
      uint8_t values[3]{};
      const auto& m=result.responses[i];
      if (parseCompletePair(m,values) &&
          (!positive_ecu || m.can_id<positive_ecu))
        positive_ecu=m.can_id;
    }
    return positive_ecu!=0;
  }

  // Decode one coherent RPM + speed pair from ONE ECU. On the first probe,
  // all responses must be complete, valid and agree on both raw PID values.
  // Choose the lowest CAN ID, independent of arrival order. An ambiguous
  // multi-ECU first probe fails closed rather than learning a random source.
  //
  // Once a source is learned, only its complete reply may satisfy the pair:
  // other ECUs cannot silently replace it or contaminate either PID. They
  // also cannot disable a working learned ECU merely by responding.
  //
  // Frames returned to BLE are virtual per-PID ELM compatibility frames,
  // NOT original CAN RAW capture evidence.
  bool decode(const DiagnosticResult& result,
              DiagnosticRawFrame output[2],
              uint32_t& responder,
              RejectReason* reject_reason=nullptr) const {
    responder=0;
    auto reject=[&](RejectReason reason) {
      if (reject_reason) *reject_reason=reason;
      return false;
    };
    if (result.status!=DiagnosticStatus::Ok)
      return reject(RejectReason::Transport);
    if (result.response_count==0 ||
        result.response_count>kDiagnosticMaxResponses)
      return reject(RejectReason::NoResponse);

    uint8_t winner[3]{};
    uint32_t chosen=0;
    uint32_t seen[kDiagnosticMaxResponses]{};
    uint8_t seen_count=0;
    for (uint8_t i=0; i<result.response_count; ++i) {
      const DiagnosticMessage& m=result.responses[i];
      if (responder_can_id_ && m.can_id!=responder_can_id_) {
        // Ignore extra responder data only AFTER learning a trusted ECU.
        continue;
      }
      // Duplicate records from the SAME candidate ECU are ambiguous, even
      // when both are valid, and must never be silently selected by order.
      for (uint8_t j=0; j<seen_count; ++j)
        if (seen[j]==m.can_id) return reject(RejectReason::DuplicateEcu);
      seen[seen_count++]=m.can_id;

      uint8_t values[3]{};
      if (!parseCompletePair(m,values))
        return reject(RejectReason::InvalidPair);
      if (!responder_can_id_ && chosen &&
          std::memcmp(values,winner,sizeof(winner))!=0)
        return reject(RejectReason::ValuesDisagree);
      if (!chosen || m.can_id<chosen) {
        chosen=m.can_id;
        std::memcpy(winner,values,sizeof(winner));
      }
    }
    if (!chosen)
      return reject(RejectReason::LearnedEcuMissing);

    // Publish neither PID until a single selected source has passed all
    // checks. Both caller and per-PID cache get identical ECU provenance.
    output[0]=DiagnosticRawFrame{};
    output[0].can_id=chosen;
    output[0].dlc=8;
    output[0].data[0]=4;
    output[0].data[1]=0x41;
    output[0].data[2]=0x0C;
    output[0].data[3]=winner[0];
    output[0].data[4]=winner[1];

    output[1]=DiagnosticRawFrame{};
    output[1].can_id=chosen;
    output[1].dlc=8;
    output[1].data[0]=3;
    output[1].data[1]=0x41;
    output[1].data[2]=0x0D;
    output[1].data[3]=winner[2];
    responder=chosen;
    if (reject_reason) *reject_reason=RejectReason::None;
    return true;
  }


 private:
  static bool parseCompletePair(const DiagnosticMessage& m,
                                uint8_t values[3]) {
    if ((m.can_id & 0x1FFFFF00U)!=0x18DAF100U ||
        m.length!=6 || m.data[0]!=0x41) return false;
    bool rpm=false,speed=false;
    uint8_t pos=1;
    while (pos<m.length) {
      const uint8_t pid=m.data[pos++];
      if (pid==0x0C && !rpm && pos+2<=m.length) {
        values[0]=m.data[pos++];
        values[1]=m.data[pos++];
        rpm=true;
      } else if (pid==0x0D && !speed && pos+1<=m.length) {
        values[2]=m.data[pos++];
        speed=true;
      } else {
        return false;
      }
    }
    return rpm && speed && pos==m.length;
  }
  Support support_ = Support::Unknown;
  uint32_t responder_can_id_ = 0;
  RejectReason last_reject_reason_ = RejectReason::None;
};

}  // namespace m5can
