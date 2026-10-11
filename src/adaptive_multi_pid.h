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

  Support support() const { return support_; }
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
    if (decode(result, frames, responder)) {
      markSupported(responder);
      return Decision::UseGrouped;
    }
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

  // Decode a complete positive response from ONE matching ECU. A partial,
  // negative, duplicated, extra-PID, stale or mismatched response must never
  // be presented as an accepted multi-PID reply.
  // The returned frames are ELM-compatible per-PID virtual responses, NOT
  // copies of physical CAN frames; the RAW CAN monitor retains the originals.
  bool decode(const DiagnosticResult& result,
              DiagnosticRawFrame output[2],
              uint32_t& responder) const {
    if (result.status != DiagnosticStatus::Ok) return false;
    for (uint8_t i = 0; i < result.response_count; ++i) {
      const DiagnosticMessage& m = result.responses[i];
      if ((m.can_id & 0x1FFFFF00U) != 0x18DAF100U ||
          (responder_can_id_ && m.can_id != responder_can_id_) ||
          m.length != 6 || m.data[0] != 0x41) continue;
      bool got_rpm = false;
      bool got_speed = false;
      uint8_t rpm_a = 0, rpm_b = 0, speed = 0;
      unsigned pos = 1;
      bool valid = true;
      while (pos < m.length) {
        const uint8_t pid = m.data[pos++];
        if (pid == 0x0C && !got_rpm && pos + 2 <= m.length) {
          rpm_a = m.data[pos++];
          rpm_b = m.data[pos++];
          got_rpm = true;
        } else if (pid == 0x0D && !got_speed && pos + 1 <= m.length) {
          speed = m.data[pos++];
          got_speed = true;
        } else {
          valid = false;
          break;
        }
      }
      if (!valid || !got_rpm || !got_speed || pos != m.length) continue;
      output[0] = DiagnosticRawFrame{};
      output[0].can_id = m.can_id;
      output[0].dlc = 8;
      output[0].data[0] = 4;
      output[0].data[1] = 0x41;
      output[0].data[2] = 0x0C;
      output[0].data[3] = rpm_a;
      output[0].data[4] = rpm_b;

      output[1] = DiagnosticRawFrame{};
      output[1].can_id = m.can_id;
      output[1].dlc = 8;
      output[1].data[0] = 3;
      output[1].data[1] = 0x41;
      output[1].data[2] = 0x0D;
      output[1].data[3] = speed;
      responder = m.can_id;
      return true;
    }
    return false;
  }

 private:
  Support support_ = Support::Unknown;
  uint32_t responder_can_id_ = 0;
};

}  // namespace m5can
