#include "serial_protocol.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "app_config.h"
#include "elm_response_formatter.h"

namespace m5can {
namespace {

void putU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void putU64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void uppercaseCopy(const char* input, char* output, size_t capacity,
                   bool remove_spaces = false) {
  if (!output || capacity == 0) return;
  size_t p = 0;
  if (input) {
    for (const char* s = input; *s && p + 1 < capacity; ++s) {
      char c = *s;
      if (remove_spaces && (c == ' ' || c == '\t')) continue;
      if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
      output[p++] = c;
    }
  }
  output[p] = '\0';
}
bool isHexChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
         (c >= 'a' && c <= 'f');
}

}  // namespace

void SerialProtocol::begin() {
  Serial.begin(cfg::kSerialBaud);
  delay(80);
  emitTextHello();
}

const char* SerialProtocol::stateName(twai_state_t state) const {
  switch (state) {
    case TWAI_STATE_STOPPED: return "STOPPED";
    case TWAI_STATE_RUNNING: return "RUNNING";
    case TWAI_STATE_BUS_OFF: return "BUS_OFF";
    case TWAI_STATE_RECOVERING: return "RECOVERING";
    default: return "UNKNOWN";
  }
}

void SerialProtocol::resetLine(LineBuffer& line) {
  line.len = 0;
  line.overflow = false;
  line.data[0] = '\0';
}

void SerialProtocol::emitTextHello() {
  Serial.printf(
      "#HELLO,%s,%s,mode=LISTEN_ONLY,bitrate=%lu,proto=TEXT,ble=%s\n",
      cfg::kFirmwareName, cfg::kFirmwareVersion,
      static_cast<unsigned long>(cfg::kCanBitrate),
      ble_ ? "ENABLED" : "DISABLED");
  Serial.println(
      "#COMMANDS,INFO STATS HELP STREAM ON STREAM OFF NATIVE ON; "
      "ATZ enters USB ELM");
}

void SerialProtocol::emitTextFrame(const CapturedFrame& frame) {
  char data_hex[17]{};
  static constexpr char kHex[] = "0123456789ABCDEF";
  if (!frame.rtr) {
    for (uint8_t i = 0; i < frame.dlc; ++i) {
      data_hex[i * 2] = kHex[(frame.data[i] >> 4) & 0x0F];
      data_hex[i * 2 + 1] = kHex[frame.data[i] & 0x0F];
    }
  }
  Serial.printf(
      frame.extended
          ? "@FRAME,%llu,E,%08lX,%u,%s,%s\n"
          : "@FRAME,%llu,S,%03lX,%u,%s,%s\n",
      static_cast<unsigned long long>(frame.timestamp_us),
      static_cast<unsigned long>(frame.identifier),
      static_cast<unsigned>(frame.dlc), data_hex,
      frame.rtr ? "RTR" : "DATA");
}

bool SerialProtocol::looksLikeElmCommand(const char* text) const {
  if (!text) return false;
  char compact[96]{};
  uppercaseCopy(text, compact, sizeof(compact), true);
  const size_t length = std::strlen(compact);
  if (length >= 2 && compact[0] == 'A' && compact[1] == 'T') return true;
  if (length != 4 && length != 6) return false;
  for (size_t i = 0; i < length; ++i) {
    if (!isHexChar(compact[i])) return false;
  }
  return true;
}

bool SerialProtocol::claimElm(ElmLink link) {
  if (native_mode_) return false;
  if (elm_link_ != ElmLink::None && elm_link_ != link) return false;

  if (!elm_mode_) {
    stream_enabled_ = false;
    elm_.reset();
    elm_mode_ = true;
    elm_link_ = link;
  }
  return elm_link_ == link;
}

void SerialProtocol::leaveElmMode() {
  // An ExitElmMode reply was already flushed with the terminal prompt.
  elm_ble_reply_len_ = 0;
  monitor_.revokeLease();
  elm_mode_ = false;
  elm_link_ = ElmLink::None;
  stream_enabled_ = false;
  elm_.reset();
}

void SerialProtocol::flushElmBleReply() {
  if (!elm_ble_reply_len_) return;
  if (ble_ && ble_->connected() && elm_link_ == ElmLink::Ble) {
    ble_->write(elm_ble_reply_, elm_ble_reply_len_);
  }
  elm_ble_reply_len_ = 0;
}

void SerialProtocol::writeElmBytes(const uint8_t* data, size_t length) {
  if (!data || !length) return;
  if (elm_link_ == ElmLink::Ble) {
    // Avoid three separate GATT notify sequences for payload, CR and '>'.
    // Do not allocate unbounded memory for multi-frame ECU replies.
    while (length) {
      if (elm_ble_reply_len_ == kElmBleReplyCapacity) flushElmBleReply();
      const size_t room = kElmBleReplyCapacity - elm_ble_reply_len_;
      const size_t chunk = std::min(room, length);
      std::memcpy(elm_ble_reply_ + elm_ble_reply_len_, data, chunk);
      elm_ble_reply_len_ += chunk;
      data += chunk;
      length -= chunk;
    }
    return;
  }
  if (elm_link_ == ElmLink::Usb) Serial.write(data, length);
}

void SerialProtocol::writeElmLine(const char* text) {
  if (text && text[0]) {
    writeElmBytes(
        reinterpret_cast<const uint8_t*>(text), std::strlen(text));
  }
  static const uint8_t kCr[] = {'\r'};
  static const uint8_t kLf[] = {'\n'};
  writeElmBytes(kCr, sizeof(kCr));
  if (elm_.linefeed()) writeElmBytes(kLf, sizeof(kLf));
}

void SerialProtocol::writeElmPrompt() {
  static const uint8_t kPrompt[] = {'>'};
  writeElmBytes(kPrompt, sizeof(kPrompt));
  flushElmBleReply();
}

void SerialProtocol::writeElmRawFrame(
    const DiagnosticRawFrame& frame) {
  char line[128]{};
  if (ElmResponseFormatter::formatRawFrame(
          frame, elm_.headers(), elm_.spaces(),
          line, sizeof(line))) {
    writeElmLine(line);
  }
}

void SerialProtocol::writeElmDiagnosticResult(
    const DiagnosticResult& result) {
  switch (result.status) {
    case DiagnosticStatus::Ok:
      for (uint8_t i = 0; i < result.raw_frame_count; ++i) {
        writeElmRawFrame(result.raw_frames[i]);
      }
      if (!result.raw_frame_count) writeElmLine("NO DATA");
      break;
    case DiagnosticStatus::NoData:
      writeElmLine("NO DATA");
      break;
    case DiagnosticStatus::LeaseRequired:
      writeElmLine("M5CAN TX LOCKED");
      break;
    case DiagnosticStatus::InvalidRequest:
      writeElmLine("?");
      break;
    case DiagnosticStatus::Busy:
    case DiagnosticStatus::RateLimited:
      writeElmLine("BUSY");
      break;
    case DiagnosticStatus::BusOff:
      writeElmLine("BUS ERROR");
      break;
    case DiagnosticStatus::IsoTpError:
    case DiagnosticStatus::DriverError:
    case DiagnosticStatus::ModeError:
    default:
      writeElmLine("CAN ERROR");
      break;
  }
}

void SerialProtocol::handleElmLine(const char* line, ElmLink link) {
  if (!claimElm(link)) {
    if (link == ElmLink::Ble) writeBleBusy();
    else Serial.println("#ERR,ELM_SESSION_ACTIVE");
    return;
  }

  char compact[96]{};
  uppercaseCopy(line, compact, sizeof(compact), true);
  if (!std::strcmp(compact, "ATZ")) monitor_.revokeLease();

  if (elm_.echo()) writeElmLine(line);

  const ElmResult result = elm_.execute(line);
  bool prompt_now = true;

  switch (result.action) {
    case ElmAction::ReplyOnly:
      if (result.reply[0]) writeElmLine(result.reply);
      break;

    case ElmAction::LeaseAcquire:
      writeElmLine(
          monitor_.acquireLease(cfg::kTxLeaseDefaultMs)
              ? "OK" : "CAN ERROR");
      break;

    case ElmAction::LeaseRevoke:
      monitor_.revokeLease();
      writeElmLine("OK");
      break;

    case ElmAction::LeaseStatus: {
      char status[96]{};
      std::snprintf(
          status, sizeof(status),
          "M5CAN TX=%s REM=%lums HDR=%08lX LINK=%s",
          monitor_.leaseRemainingMs() ? "LEASED" : "LOCKED",
          static_cast<unsigned long>(monitor_.leaseRemainingMs()),
          static_cast<unsigned long>(elm_.effectiveHeader()),
          elm_link_ == ElmLink::Ble ? "BLE" : "USB");
      writeElmLine(status);
      break;
    }

    case ElmAction::VehicleRead: {
      DiagnosticRequest request{};
      request.can_id = result.request.can_id;
      request.length = result.request.length;
      std::memcpy(request.data, result.request.data, request.length);

      const uint32_t configured_timeout =
          elm_.timeout4ms()
              ? static_cast<uint32_t>(elm_.timeout4ms()) * 4U
              : 200U;
      request.timeout_ms =
          std::max<uint32_t>(
              20, std::min<uint32_t>(configured_timeout, 1000));

      if (!monitor_.submitQuery(request)) {
        writeElmLine("BUSY");
      } else {
        elm_query_pending_ = true;
        prompt_now = false;
      }
      break;
    }

    case ElmAction::ExitElmMode:
      if (result.reply[0]) writeElmLine(result.reply);
      break;
  }

  if (prompt_now) writeElmPrompt();
  if (result.action == ElmAction::ExitElmMode) leaveElmMode();
}

void SerialProtocol::writeBleBusy() {
  if (!ble_ || !ble_->connected()) return;
  static const uint8_t kBusy[] = {'B','U','S','Y','\r','>'};
  ble_->write(kBusy, sizeof(kBusy));
}

void SerialProtocol::handleBleDisconnect() {
  if (!ble_ || !ble_->takeDisconnectEvent()) return;

  // Never forward a previous connection's buffered response to the next one.
  elm_ble_reply_len_ = 0;
  // A dropped wireless link must immediately remove authorization.
  monitor_.revokeLease();
  resetLine(ble_line_);

  if (elm_link_ == ElmLink::Ble) {
    elm_link_ = ElmLink::None;
    elm_mode_ = false;
    elm_.reset();
  }
  // If a CAN owner transaction is still completing, keep
  // elm_query_pending_ set until its result is drained below.
}

void SerialProtocol::handleUsbLine(const char* line) {
  if (elm_mode_ && elm_link_ == ElmLink::Usb) {
    handleElmLine(line, ElmLink::Usb);
    return;
  }

  if (looksLikeElmCommand(line)) {
    if (elm_link_ == ElmLink::Ble) {
      Serial.println("#ERR,BLE_ELM_ACTIVE");
      return;
    }
    handleElmLine(line, ElmLink::Usb);
    return;
  }

  char command[129]{};
  uppercaseCopy(line, command, sizeof(command));
  if (!std::strcmp(command, "INFO")) {
    emitTextHello();
  } else if (!std::strcmp(command, "STATS")) {
    emitStats(true);
  } else if (!std::strcmp(command, "STREAM ON")) {
    stream_enabled_ = true;
    Serial.println("#OK,STREAM,ON");
  } else if (!std::strcmp(command, "STREAM OFF")) {
    stream_enabled_ = false;
    Serial.println("#OK,STREAM,OFF");
  } else if (!std::strcmp(command, "NATIVE ON")) {
    if (elm_link_ != ElmLink::None) {
      Serial.println("#ERR,ELM_SESSION_ACTIVE");
    } else {
      enterNativeMode();
    }
  } else if (!std::strcmp(command, "HELP") ||
             !std::strcmp(command, "?")) {
    Serial.println(
        "#HELP,INFO STATS HELP STREAM ON STREAM OFF NATIVE ON; "
        "BLE exposes ELM only");
  } else if (command[0]) {
    Serial.printf("#ERR,UNKNOWN_COMMAND,%s\n", command);
  }
}

void SerialProtocol::processUsbByte(uint8_t byte) {
  if (native_mode_) {
    processNativeByte(byte);
    return;
  }

  const char c = static_cast<char>(byte);
  if (c == '\n' || c == '\r') {
    if (usb_line_.overflow) {
      if (elm_mode_ && elm_link_ == ElmLink::Usb) {
        writeElmLine("?");
        writeElmPrompt();
      } else {
        Serial.println("#ERR,LINE_TOO_LONG");
      }
    } else if (usb_line_.len) {
      usb_line_.data[usb_line_.len] = '\0';
      handleUsbLine(usb_line_.data);
    }
    resetLine(usb_line_);
    return;
  }

  if (c < 0x20 || c > 0x7E) return;
  if (usb_line_.len < sizeof(usb_line_.data) - 1) {
    usb_line_.data[usb_line_.len++] = c;
  } else {
    usb_line_.overflow = true;
  }
}

void SerialProtocol::processBleByte(uint8_t byte) {
  const char c = static_cast<char>(byte);
  if (c == '\n' || c == '\r') {
    if (ble_line_.overflow) {
      if (elm_link_ == ElmLink::None ||
          elm_link_ == ElmLink::Ble) {
        if (claimElm(ElmLink::Ble)) {
          writeElmLine("?");
          writeElmPrompt();
        }
      } else {
        writeBleBusy();
      }
    } else if (ble_line_.len) {
      ble_line_.data[ble_line_.len] = '\0';
      if (native_mode_ || elm_link_ == ElmLink::Usb) {
        writeBleBusy();
      } else {
        handleElmLine(ble_line_.data, ElmLink::Ble);
      }
    }
    resetLine(ble_line_);
    return;
  }

  if (c < 0x20 || c > 0x7E) return;
  if (ble_line_.len < sizeof(ble_line_.data) - 1) {
    ble_line_.data[ble_line_.len++] = c;
  } else {
    ble_line_.overflow = true;
  }
}

void SerialProtocol::pollUsbInput() {
  constexpr size_t kMaxBytesPerPoll = 256;
  size_t processed = 0;
  while (Serial.available() > 0 &&
         processed < kMaxBytesPerPoll) {
    const int value = Serial.read();
    if (value < 0) break;
    processUsbByte(static_cast<uint8_t>(value));
    ++processed;
  }
}

void SerialProtocol::pollBleInput() {
  if (!ble_ || !ble_->connected()) return;

  constexpr size_t kMaxBytesPerPoll = 256;
  size_t processed = 0;
  while (ble_->available() > 0 &&
         processed < kMaxBytesPerPoll) {
    const int value = ble_->read();
    if (value < 0) break;
    processBleByte(static_cast<uint8_t>(value));
    ++processed;
  }
}

void SerialProtocol::emitNativePacket(
    native::PacketType type, const uint8_t* payload,
    uint16_t payload_len, uint8_t flags) {
  uint8_t wire[cfg::kNativeMaxEncodedPacket]{};
  const uint16_t sequence = native_tx_sequence_;
  const size_t n = native::encodePacket(
      type, flags, sequence, payload, payload_len,
      wire, sizeof(wire));
  if (!n) return;
  Serial.write(wire, n);
  native_tx_sequence_ =
      static_cast<uint16_t>(native_tx_sequence_ + 1);
}

void SerialProtocol::emitNativeHello() {
  uint8_t payload[cfg::kNativeMaxPayload]{};
  size_t p = 0;
  payload[p++] = cfg::kNativeProtocolVersion;
  putU32(&payload[p], cfg::kCanBitrate); p += 4;
  payload[p++] = 0;
  payload[p++] = stream_enabled_ ? 1 : 0;

  const size_t name_len = std::strlen(cfg::kFirmwareName);
  const size_t ver_len = std::strlen(cfg::kFirmwareVersion);
  if (name_len > 31 || ver_len > 31 ||
      p + name_len + ver_len + 2 > sizeof(payload)) return;
  payload[p++] = static_cast<uint8_t>(name_len);
  std::memcpy(&payload[p], cfg::kFirmwareName, name_len);
  p += name_len;
  payload[p++] = static_cast<uint8_t>(ver_len);
  std::memcpy(&payload[p], cfg::kFirmwareVersion, ver_len);
  p += ver_len;
  emitNativePacket(
      native::PacketType::Hello, payload,
      static_cast<uint16_t>(p));
}

void SerialProtocol::emitNativeStats() {
  const CanStats s = monitor_.stats();
  uint8_t payload[72]{};
  size_t p = 0;
  putU64(&payload[p], s.rx_frames); p += 8;
  putU64(&payload[p], s.std_frames); p += 8;
  putU64(&payload[p], s.ext_frames); p += 8;
  putU64(&payload[p], s.rtr_frames); p += 8;
  putU64(&payload[p], s.app_queue_drops); p += 8;
  putU32(&payload[p], s.driver_rx_missed); p += 4;
  putU32(&payload[p], s.driver_rx_overrun); p += 4;
  putU32(&payload[p], s.driver_bus_error); p += 4;
  putU32(&payload[p], s.driver_arb_lost); p += 4;
  putU32(&payload[p], s.rx_queue_depth); p += 4;
  payload[p++] = static_cast<uint8_t>(s.state);
  putU64(&payload[p], native_rx_errors_); p += 8;
  emitNativePacket(
      native::PacketType::Stats, payload,
      static_cast<uint16_t>(p));
}

void SerialProtocol::emitNativeEvent(const char* text) {
  if (!text) return;
  const size_t n = strnlen(text, cfg::kNativeMaxPayload);
  emitNativePacket(
      native::PacketType::Event,
      reinterpret_cast<const uint8_t*>(text),
      static_cast<uint16_t>(n));
}

void SerialProtocol::emitNativeReply(
    native::Command command, native::Status status) {
  const uint8_t payload[2] = {
      static_cast<uint8_t>(command),
      static_cast<uint8_t>(status)};
  emitNativePacket(
      native::PacketType::CommandReply,
      payload, sizeof(payload));
}

void SerialProtocol::emitNativeFrame(
    const CapturedFrame& frame) {
  uint8_t payload[32]{};
  const size_t n = native::buildCanFramePayload(
      frame, payload, sizeof(payload));
  if (n) {
    emitNativePacket(
        native::PacketType::CanFrame,
        payload, static_cast<uint16_t>(n));
  }
}

void SerialProtocol::emitFrame(
    const CapturedFrame& frame) {
  if (elm_mode_ || !stream_enabled_) return;
  if (native_mode_) emitNativeFrame(frame);
  else emitTextFrame(frame);
}

void SerialProtocol::emitStats(bool forced) {
  if (elm_mode_) return;
  const uint32_t now = millis();
  if (!forced &&
      now - last_stats_ms_ < cfg::kStatsEmitMs) return;
  last_stats_ms_ = now;

  if (native_mode_) {
    emitNativeStats();
    return;
  }

  const CanStats s = monitor_.stats();
  Serial.printf(
      "#STATS,rx=%llu,std=%llu,ext=%llu,rtr=%llu,"
      "drop=%llu,tx=%llu/%llu,q=%llu,to=%llu,"
      "missed=%lu,overrun=%lu,buserr=%lu,arb_lost=%lu,"
      "driver_q=%lu,state=%s,lease_ms=%lu,ble=%s,stream=%s\n",
      static_cast<unsigned long long>(s.rx_frames),
      static_cast<unsigned long long>(s.std_frames),
      static_cast<unsigned long long>(s.ext_frames),
      static_cast<unsigned long long>(s.rtr_frames),
      static_cast<unsigned long long>(s.app_queue_drops),
      static_cast<unsigned long long>(s.tx_success),
      static_cast<unsigned long long>(s.tx_attempts),
      static_cast<unsigned long long>(s.query_count),
      static_cast<unsigned long long>(s.query_timeout),
      static_cast<unsigned long>(s.driver_rx_missed),
      static_cast<unsigned long>(s.driver_rx_overrun),
      static_cast<unsigned long>(s.driver_bus_error),
      static_cast<unsigned long>(s.driver_arb_lost),
      static_cast<unsigned long>(s.rx_queue_depth),
      stateName(s.state),
      static_cast<unsigned long>(monitor_.leaseRemainingMs()),
      ble_ && ble_->connected() ? "LINK" : "WAIT",
      stream_enabled_ ? "ON" : "OFF");
}

void SerialProtocol::enterNativeMode() {
  if (native_mode_ || elm_link_ != ElmLink::None) return;
  monitor_.revokeLease();
  stream_enabled_ = false;
  Serial.println("#OK,NATIVE,ON,COBS+CRC32,version=1");
  Serial.flush();
  native_input_len_ = 0;
  native_discard_until_delimiter_ = false;
  native_mode_ = true;
  emitNativeHello();
  emitNativeEvent("BLE_ELM_AVAILABLE_SEPARATELY");
}

void SerialProtocol::leaveNativeMode() {
  native_mode_ = false;
  stream_enabled_ = false;
  native_input_len_ = 0;
  native_discard_until_delimiter_ = false;
  Serial.println("#OK,NATIVE,OFF,TEXT");
  emitTextHello();
}

void SerialProtocol::handleNativePacket(
    const native::DecodedPacket& packet) {
  if (packet.type != native::PacketType::Command ||
      packet.payload_len != 1) {
    ++native_rx_errors_;
    emitNativeEvent("BAD_HOST_PACKET");
    return;
  }

  const auto command =
      static_cast<native::Command>(packet.payload[0]);
  switch (command) {
    case native::Command::Ping:
      emitNativeReply(command, native::Status::Ok);
      break;
    case native::Command::GetInfo:
      emitNativeReply(command, native::Status::Ok);
      emitNativeHello();
      break;
    case native::Command::GetStats:
      emitNativeReply(command, native::Status::Ok);
      emitNativeStats();
      break;
    case native::Command::StartStream:
      stream_enabled_ = true;
      emitNativeReply(command, native::Status::Ok);
      emitNativeEvent("STREAM_ON");
      break;
    case native::Command::StopStream:
      stream_enabled_ = false;
      emitNativeReply(command, native::Status::Ok);
      emitNativeEvent("STREAM_OFF");
      break;
    case native::Command::TextMode:
      emitNativeReply(command, native::Status::Ok);
      Serial.flush();
      leaveNativeMode();
      break;
    default:
      emitNativeReply(command, native::Status::BadCommand);
      break;
  }
}

void SerialProtocol::processNativeByte(uint8_t byte) {
  if (native_discard_until_delimiter_) {
    if (byte == 0) {
      native_discard_until_delimiter_ = false;
      native_input_len_ = 0;
    }
    return;
  }

  if (byte == 0) {
    if (!native_input_len_) return;
    native::DecodedPacket packet{};
    if (native::decodePacket(
            native_input_, native_input_len_, packet)) {
      handleNativePacket(packet);
    } else {
      ++native_rx_errors_;
      emitNativeEvent("CRC_OR_FRAME_ERROR");
    }
    native_input_len_ = 0;
    return;
  }

  if (native_input_len_ >= sizeof(native_input_)) {
    ++native_rx_errors_;
    native_input_len_ = 0;
    native_discard_until_delimiter_ = true;
    return;
  }

  native_input_[native_input_len_++] = byte;
}

void SerialProtocol::pollInput() {
  handleBleDisconnect();

  if (elm_query_pending_) {
    DiagnosticResult result{};
    if (monitor_.pollQueryResult(result)) {
      elm_query_pending_ = false;
      if (elm_link_ != ElmLink::None) {
        writeElmDiagnosticResult(result);
        writeElmPrompt();
      }
    }
    if (elm_query_pending_) return;
  }

  // BLE first so an iPad command can claim an idle ELM session
  // deterministically. USB remains available for debug/native use.
  pollBleInput();
  pollUsbInput();
}

}  // namespace m5can
