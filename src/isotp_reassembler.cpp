#include "isotp_reassembler.h"

#include <algorithm>
#include <cstring>

namespace m5can {

void IsoTpAssembler::begin(const DiagnosticRequest& request, uint32_t cf_timeout_ms) {
  request_ = request;
  cf_timeout_ms_ = std::max<uint32_t>(20, std::min<uint32_t>(cf_timeout_ms, 1000));
  for (auto& session : sessions_) session = Session{};
}

uint32_t IsoTpAssembler::flowControlIdForResponse(uint32_t response_id) {
  if (!DiagnosticPolicy::allowedResponseId(response_id)) return 0;
  return 0x18DA00F1U | ((response_id & 0xFFU) << 8);
}

IsoTpAssembler::Session* IsoTpAssembler::find(uint32_t response_id) {
  for (auto& session : sessions_) {
    if (session.active && session.response_id == response_id) return &session;
  }
  return nullptr;
}

IsoTpAssembler::Session* IsoTpAssembler::allocate(uint32_t response_id) {
  for (auto& session : sessions_) {
    if (!session.active) {
      session = Session{};
      session.active = true;
      session.response_id = response_id;
      session.message.can_id = response_id;
      return &session;
    }
  }
  return nullptr;
}

bool IsoTpAssembler::initialPayloadMatches(const uint8_t* payload, size_t length) const {
  return DiagnosticPolicy::responseMatches(request_, payload, length);
}

IsoTpEvent IsoTpAssembler::feed(uint32_t can_id, bool extended, bool rtr,
                                const uint8_t* data, uint8_t dlc,
                                uint64_t timestamp_us) {
  IsoTpEvent event{};
  if (!extended || rtr || !data || dlc == 0 ||
      !DiagnosticPolicy::allowedResponseId(can_id)) {
    return event;
  }

  const uint8_t pci_type = static_cast<uint8_t>(data[0] >> 4);
  if (pci_type == 0x0) {
    const uint8_t payload_len = static_cast<uint8_t>(data[0] & 0x0F);
    if (payload_len == 0 || payload_len > 7 ||
        static_cast<uint8_t>(payload_len + 1) > dlc) return event;
    if (!initialPayloadMatches(&data[1], payload_len)) return event;

    if (payload_len >= 3 && data[1] == 0x7F &&
        data[2] == request_.data[0] && data[3] == 0x78) {
      event.type = IsoTpEventType::ResponsePending;
      event.response_id = can_id;
      return event;
    }

    event.type = IsoTpEventType::Complete;
    event.response_id = can_id;
    event.message.can_id = can_id;
    event.message.length = payload_len;
    std::memcpy(event.message.data, &data[1], payload_len);
    return event;
  }

  if (pci_type == 0x1) {
    if (dlc < 4) return event;
    const uint16_t total_len =
        static_cast<uint16_t>(((data[0] & 0x0F) << 8) | data[1]);
    const size_t available = std::min<size_t>(dlc - 2, total_len);
    if (!initialPayloadMatches(&data[2], available)) return event;

    event.response_id = can_id;
    event.flow_control_id = flowControlIdForResponse(can_id);

    if (find(can_id)) {
      abort(can_id);
      event.type = IsoTpEventType::Collision;
      return event;
    }
    if (total_len <= 7 || total_len > kDiagnosticMaxPayload) {
      event.type = IsoTpEventType::Overflow;
      return event;
    }

    Session* session = allocate(can_id);
    if (!session) {
      event.type = IsoTpEventType::CapacityError;
      return event;
    }

    session->expected_length = total_len;
    session->received = static_cast<uint16_t>(available);
    session->next_sequence = 1;
    session->deadline_us =
        timestamp_us + static_cast<uint64_t>(cf_timeout_ms_) * 1000ULL;
    session->message.length = session->received;
    std::memcpy(session->message.data, &data[2], available);

    if (session->received >= session->expected_length) {
      event.type = IsoTpEventType::Complete;
      event.message = session->message;
      session->active = false;
      return event;
    }

    event.type = IsoTpEventType::NeedFlowControl;
    return event;
  }

  if (pci_type == 0x2) {
    Session* session = find(can_id);
    if (!session || dlc < 2) return event;

    const uint8_t sequence = static_cast<uint8_t>(data[0] & 0x0F);
    if (sequence != session->next_sequence) {
      session->active = false;
      event.type = IsoTpEventType::SequenceError;
      event.response_id = can_id;
      return event;
    }

    const uint16_t remaining =
        static_cast<uint16_t>(session->expected_length - session->received);
    const uint16_t copy_len =
        static_cast<uint16_t>(std::min<size_t>(dlc - 1, remaining));
    std::memcpy(&session->message.data[session->received], &data[1], copy_len);
    session->received = static_cast<uint16_t>(session->received + copy_len);
    session->message.length = session->received;
    session->next_sequence = static_cast<uint8_t>((session->next_sequence + 1) & 0x0F);
    session->deadline_us =
        timestamp_us + static_cast<uint64_t>(cf_timeout_ms_) * 1000ULL;

    if (session->received >= session->expected_length) {
      event.type = IsoTpEventType::Complete;
      event.response_id = can_id;
      event.message = session->message;
      session->active = false;
    } else {
      event.type = IsoTpEventType::Progress;
      event.response_id = can_id;
    }
    return event;
  }

  return event;
}

uint8_t IsoTpAssembler::expire(uint64_t now_us) {
  uint8_t count = 0;
  for (auto& session : sessions_) {
    if (session.active && now_us >= session.deadline_us) {
      session.active = false;
      ++count;
    }
  }
  return count;
}

bool IsoTpAssembler::hasActiveSessions() const {
  for (const auto& session : sessions_) {
    if (session.active) return true;
  }
  return false;
}

void IsoTpAssembler::abort(uint32_t response_id) {
  Session* session = find(response_id);
  if (session) session->active = false;
}

}  // namespace m5can
