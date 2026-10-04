#include "serial_protocol.h"

#include <cstring>

#include "app_config.h"

namespace m5can {
namespace {

void putU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void putU64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void uppercaseCopy(const char* input, char* output, size_t capacity) {
  if (!output || capacity == 0) return;
  size_t p = 0;
  if (input) {
    for (const char* s = input; *s && p + 1 < capacity; ++s) {
      const char c = *s;
      output[p++] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
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

void SerialProtocol::emitTextHello() {
  Serial.printf("#HELLO,%s,%s,mode=LISTEN_ONLY,bitrate=%lu,tx=DISABLED,proto=TEXT\n",
                cfg::kFirmwareName, cfg::kFirmwareVersion,
                static_cast<unsigned long>(cfg::kCanBitrate));
  Serial.println("#COMMANDS,INFO STATS HELP STREAM ON STREAM OFF NATIVE ON; ATZ enters ELM");
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
  Serial.printf(frame.extended
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
  size_t p = 0;
  for (const char* s = text; *s && p + 1 < sizeof(compact); ++s) {
    if (*s == ' ' || *s == '\t') continue;
    compact[p++] = *s;
  }
  compact[p] = '\0';
  if (p >= 2 &&
      (compact[0] == 'A' || compact[0] == 'a') &&
      (compact[1] == 'T' || compact[1] == 't')) return true;
  if (p != 4 && p != 6) return false;
  for (size_t i = 0; i < p; ++i) {
    if (!isHexChar(compact[i])) return false;
  }
  return true;
}

void SerialProtocol::enterElmMode() {
  if (elm_mode_) return;
  stream_enabled_ = false;
  native_mode_ = false;
  elm_mode_ = true;
  elm_.reset();
}

void SerialProtocol::leaveElmMode() {
  elm_mode_ = false;
  stream_enabled_ = false;
}

void SerialProtocol::writeElmLine(const char* text) {
  if (text && text[0]) {
    Serial.write(reinterpret_cast<const uint8_t*>(text), std::strlen(text));
  }
  static const uint8_t kCr[] = {'\r'};
  static const uint8_t kLf[] = {'\n'};
  Serial.write(kCr, sizeof(kCr));
  if (elm_.linefeed()) Serial.write(kLf, sizeof(kLf));
}

void SerialProtocol::writeElmPrompt() {
  static const uint8_t kPrompt[] = {'>'};
  Serial.write(kPrompt, sizeof(kPrompt));
}

void SerialProtocol::handleElmLine() {
  const bool echo_before = elm_.echo();
  if (echo_before) writeElmLine(text_input_);

  const ElmResult result = elm_.execute(text_input_);
  if (result.reply[0]) writeElmLine(result.reply);
  writeElmPrompt();

  if (result.action == ElmAction::ExitElmMode) leaveElmMode();
}

void SerialProtocol::emitNativePacket(native::PacketType type, const uint8_t* payload,
                                      uint16_t payload_len, uint8_t flags) {
  uint8_t wire[cfg::kNativeMaxEncodedPacket]{};
  const uint16_t sequence = native_tx_sequence_;
  const size_t n = native::encodePacket(type, flags, sequence,
                                        payload, payload_len, wire, sizeof(wire));
  if (!n) return;
  Serial.write(wire, n);
  native_tx_sequence_ = static_cast<uint16_t>(native_tx_sequence_ + 1);
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
  if (name_len > 31 || ver_len > 31 || p + name_len + ver_len + 2 > sizeof(payload)) return;
  payload[p++] = static_cast<uint8_t>(name_len);
  std::memcpy(&payload[p], cfg::kFirmwareName, name_len); p += name_len;
  payload[p++] = static_cast<uint8_t>(ver_len);
  std::memcpy(&payload[p], cfg::kFirmwareVersion, ver_len); p += ver_len;
  emitNativePacket(native::PacketType::Hello, payload, static_cast<uint16_t>(p));
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
  emitNativePacket(native::PacketType::Stats, payload, static_cast<uint16_t>(p));
}

void SerialProtocol::emitNativeEvent(const char* text) {
  if (!text) return;
  const size_t n = strnlen(text, cfg::kNativeMaxPayload);
  emitNativePacket(native::PacketType::Event,
                   reinterpret_cast<const uint8_t*>(text),
                   static_cast<uint16_t>(n));
}

void SerialProtocol::emitNativeReply(native::Command command, native::Status status) {
  const uint8_t payload[2] = {
      static_cast<uint8_t>(command), static_cast<uint8_t>(status)};
  emitNativePacket(native::PacketType::CommandReply, payload, sizeof(payload));
}

void SerialProtocol::emitNativeFrame(const CapturedFrame& frame) {
  uint8_t payload[32]{};
  const size_t n = native::buildCanFramePayload(frame, payload, sizeof(payload));
  if (n) emitNativePacket(native::PacketType::CanFrame, payload, static_cast<uint16_t>(n));
}

void SerialProtocol::emitFrame(const CapturedFrame& frame) {
  if (elm_mode_ || !stream_enabled_) return;
  if (native_mode_) emitNativeFrame(frame);
  else emitTextFrame(frame);
}

void SerialProtocol::emitStats(bool forced) {
  if (elm_mode_) return;
  const uint32_t now = millis();
  if (!forced && now - last_stats_ms_ < cfg::kStatsEmitMs) return;
  last_stats_ms_ = now;
  if (native_mode_) {
    emitNativeStats();
    return;
  }
  const CanStats s = monitor_.stats();
  Serial.printf("#STATS,rx=%llu,std=%llu,ext=%llu,rtr=%llu,drop=%llu,missed=%lu,overrun=%lu,buserr=%lu,arb_lost=%lu,driver_q=%lu,state=%s,stream=%s\n",
                static_cast<unsigned long long>(s.rx_frames),
                static_cast<unsigned long long>(s.std_frames),
                static_cast<unsigned long long>(s.ext_frames),
                static_cast<unsigned long long>(s.rtr_frames),
                static_cast<unsigned long long>(s.app_queue_drops),
                static_cast<unsigned long>(s.driver_rx_missed),
                static_cast<unsigned long>(s.driver_rx_overrun),
                static_cast<unsigned long>(s.driver_bus_error),
                static_cast<unsigned long>(s.driver_arb_lost),
                static_cast<unsigned long>(s.rx_queue_depth),
                stateName(s.state), stream_enabled_ ? "ON" : "OFF");
}

void SerialProtocol::enterNativeMode() {
  if (native_mode_) return;
  elm_mode_ = false;
  stream_enabled_ = false;
  Serial.println("#OK,NATIVE,ON,COBS+CRC32,version=1");
  Serial.flush();
  native_input_len_ = 0;
  native_discard_until_delimiter_ = false;
  native_mode_ = true;
  emitNativeHello();
  emitNativeEvent("TX_DISABLED_PHASE3A");
}

void SerialProtocol::leaveNativeMode() {
  native_mode_ = false;
  stream_enabled_ = false;
  native_input_len_ = 0;
  native_discard_until_delimiter_ = false;
  Serial.println("#OK,NATIVE,OFF,TEXT");
  emitTextHello();
}

void SerialProtocol::handleTextLine() {
  if (looksLikeElmCommand(text_input_)) {
    enterElmMode();
    handleElmLine();
    return;
  }

  char command[129]{};
  uppercaseCopy(text_input_, command, sizeof(command));
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
    enterNativeMode();
  } else if (!std::strcmp(command, "HELP") || !std::strcmp(command, "?")) {
    Serial.println("#HELP,INFO STATS HELP STREAM ON STREAM OFF NATIVE ON; ATZ enters ELM");
  } else if (text_input_len_) {
    Serial.printf("#ERR,UNKNOWN_COMMAND,%s\n", command);
  }
}

void SerialProtocol::handleNativePacket(const native::DecodedPacket& packet) {
  if (packet.type != native::PacketType::Command || packet.payload_len != 1) {
    ++native_rx_errors_;
    emitNativeEvent("BAD_HOST_PACKET");
    return;
  }
  const auto command = static_cast<native::Command>(packet.payload[0]);
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
    if (native::decodePacket(native_input_, native_input_len_, packet)) {
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
  constexpr size_t kMaxBytesPerPoll = 256;
  size_t processed = 0;
  while (Serial.available() > 0 && processed < kMaxBytesPerPoll) {
    const uint8_t byte = static_cast<uint8_t>(Serial.read());
    ++processed;
    if (native_mode_) {
      processNativeByte(byte);
      continue;
    }

    const char c = static_cast<char>(byte);
    if (c == '\n' || c == '\r') {
      if (text_overflow_) {
        if (elm_mode_) {
          writeElmLine("?");
          writeElmPrompt();
        } else {
          Serial.println("#ERR,LINE_TOO_LONG");
        }
      } else if (text_input_len_) {
        if (elm_mode_) handleElmLine();
        else handleTextLine();
      }
      text_input_len_ = 0;
      text_overflow_ = false;
      continue;
    }
    if (c < 0x20 || c > 0x7E) continue;
    if (text_input_len_ < sizeof(text_input_) - 1) {
      text_input_[text_input_len_++] = c;
    } else {
      text_overflow_ = true;
    }
  }
}

}  // namespace m5can
