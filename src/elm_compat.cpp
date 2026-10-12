#include "elm_compat.h"
#include "app_config.h"

#include <cstdio>
#include <cstring>

namespace m5can {
namespace {

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

void ElmCompat::reset() {
  echo_ = true;
  linefeed_ = false;
  spaces_ = false;
  headers_ = true;
  allow_long_ = true;
  auto_format_ = true;
  flow_control_ = true;
  protocol_ = 7;
  priority_ = 0x18;
  lower_header_ = 0xDB33F1;
  adaptive_timing_ = 1;
  timeout_4ms_ = 0x32;
}

void ElmCompat::normalize(const char* input, char* output, size_t capacity) {
  if (!output || capacity == 0) return;
  size_t p = 0;
  if (input) {
    for (const char* s = input; *s && p + 1 < capacity; ++s) {
      const char c = *s;
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
      output[p++] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    }
  }
  output[p] = '\0';
}

bool ElmCompat::startsWith(const char* text, const char* prefix) {
  return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

void ElmCompat::setReply(ElmResult& result, const char* text) {
  std::snprintf(result.reply, sizeof(result.reply), "%s", text ? text : "");
}

bool ElmCompat::parseHexByte(const char* text, uint8_t& value) {
  if (!text || !text[0] || !text[1] || text[2]) return false;
  const int hi = hexNibble(text[0]);
  const int lo = hexNibble(text[1]);
  if (hi < 0 || lo < 0) return false;
  value = static_cast<uint8_t>((hi << 4) | lo);
  return true;
}

bool ElmCompat::parseHexBytes(const char* text, uint8_t* output, size_t capacity,
                              uint8_t& length) {
  length = 0;
  if (!text) return false;
  const size_t chars = std::strlen(text);
  if (chars == 0 || (chars & 1U) || chars / 2 > capacity) return false;
  for (size_t i = 0; i < chars; i += 2) {
    char byte_text[3] = {text[i], text[i + 1], '\0'};
    uint8_t value = 0;
    if (!parseHexByte(byte_text, value)) return false;
    output[length++] = value;
  }
  return true;
}

bool ElmCompat::validateVehicleRead(const uint8_t* bytes, uint8_t length) const {
  if (!bytes) return false;
  if (length == 2 && (bytes[0] == 0x01 || bytes[0] == 0x09)) return true;
  if (length == 3 && bytes[0] == 0x22) return true;
  return false;
}

ElmResult ElmCompat::execute(const char* command) {
  ElmResult result{};
  char cmd[96]{};
  normalize(command, cmd, sizeof(cmd));
  if (!cmd[0]) {
    setReply(result, "");
    return result;
  }

  if (!std::strcmp(cmd, "ATZ")) {
    reset();
    setReply(result, "M5CAN v0.4 ELM-CAN compatible");
    return result;
  }
  if (!std::strcmp(cmd, "ATI")) {
    setReply(result, "M5CAN v0.4 ELM-CAN compatible");
    return result;
  }
  if (!std::strcmp(cmd, "AT@1")) {
    setReply(result, "M5CAN-Dial");
    return result;
  }
  if (!std::strcmp(cmd, "ATDP")) {
    setReply(result, "ISO 15765-4 (CAN 29/500)");
    return result;
  }
  if (!std::strcmp(cmd, "ATDPN")) {
    setReply(result, "A7");
    return result;
  }

  if (!std::strcmp(cmd, "ATE0") || !std::strcmp(cmd, "ATE1")) {
    echo_ = cmd[3] == '1';
    setReply(result, "OK");
    return result;
  }
  if (!std::strcmp(cmd, "ATL0") || !std::strcmp(cmd, "ATL1")) {
    linefeed_ = cmd[3] == '1';
    setReply(result, "OK");
    return result;
  }
  if (!std::strcmp(cmd, "ATS0") || !std::strcmp(cmd, "ATS1")) {
    spaces_ = cmd[3] == '1';
    setReply(result, "OK");
    return result;
  }
  if (!std::strcmp(cmd, "ATH0") || !std::strcmp(cmd, "ATH1")) {
    headers_ = cmd[3] == '1';
    setReply(result, "OK");
    return result;
  }
  if (!std::strcmp(cmd, "ATAL")) {
    allow_long_ = true;
    setReply(result, "OK");
    return result;
  }
  if (!std::strcmp(cmd, "ATCAF0") || !std::strcmp(cmd, "ATCAF1")) {
    auto_format_ = cmd[5] == '1';
    setReply(result, "OK");
    return result;
  }
  if (!std::strcmp(cmd, "ATCFC0") || !std::strcmp(cmd, "ATCFC1")) {
    flow_control_ = cmd[5] == '1';
    setReply(result, "OK");
    return result;
  }
  if (!std::strcmp(cmd, "ATSP7")) {
    protocol_ = 7;
    setReply(result, "OK");
    return result;
  }

  if (startsWith(cmd, "ATCP")) {
    uint8_t value = 0;
    if (std::strlen(cmd) != 6 || !parseHexByte(cmd + 4, value) || value > 0x1F) {
      setReply(result, "?");
      return result;
    }
    priority_ = value;
    setReply(result, "OK");
    return result;
  }

  if (startsWith(cmd, "ATSH")) {
    if (std::strlen(cmd) != 10) {
      setReply(result, "?");
      return result;
    }
    uint8_t bytes[3]{};
    uint8_t length = 0;
    if (!parseHexBytes(cmd + 4, bytes, sizeof(bytes), length) || length != 3) {
      setReply(result, "?");
      return result;
    }
    lower_header_ = (static_cast<uint32_t>(bytes[0]) << 16) |
                    (static_cast<uint32_t>(bytes[1]) << 8) |
                    static_cast<uint32_t>(bytes[2]);
    setReply(result, "OK");
    return result;
  }

  if (startsWith(cmd, "ATAT")) {
    if (std::strlen(cmd) != 5 || cmd[4] < '0' || cmd[4] > '2') {
      setReply(result, "?");
      return result;
    }
    adaptive_timing_ = static_cast<uint8_t>(cmd[4] - '0');
    setReply(result, "OK");
    return result;
  }

  if (startsWith(cmd, "ATST")) {
    uint8_t value = 0;
    if (std::strlen(cmd) != 6 || !parseHexByte(cmd + 4, value)) {
      setReply(result, "?");
      return result;
    }
    timeout_4ms_ = value;
    setReply(result, "OK");
    return result;
  }

  // Explicitly probed by iPad only after M5CAN identity verification.
  // This capability string is deliberately unique to the batch-enabled build.
  if (!std::strcmp(cmd, "ATM5CAP")) {
    // Firmware version and protocol version are intentionally independent.
    // New firmware can preserve V1 compatibility or advertise V2 explicitly.
    std::snprintf(result.reply, sizeof(result.reply),
                  "M5CAN-CAPS PROTO=1.1 FW=%s BATCH=16 "
                  "OPS=OBD01,UDS22 STREAM=0 LEASE=IMPLICIT",
                  cfg::kFirmwareVersion);
    return result;
  }
  if (startsWith(cmd, "ATM5B")) {
    // One BLE request schedules 1..16 read-only CAN transactions on the
    // owner task. ECU 01=physical UDS22; 00=functional supported OBD01.
    // Syntax: ATM5B01:2012,E480 ; ATM5B00:010C,010D,0105,015B,019A
    const size_t len = std::strlen(cmd);
    if (len < 12 || cmd[7] != ':' ||
        !((cmd[5] == '0' && cmd[6] == '1') ||
          (cmd[5] == '0' && cmd[6] == '0'))) {
      setReply(result, "?");
      return result;
    }
    const bool uds = cmd[6] == '1';
    const uint32_t header = uds ? 0x18DA01F1U : 0x18DB33F1U;
    const char* cursor = cmd + 8;
    while (*cursor) {
      if (result.batch_count >= ElmResult::kBatchMaxIDs) {
        setReply(result, "?");
        return result;
      }
      char word[5]{};
      for (int i = 0; i < 4; ++i) {
        if (!cursor[i] || (cursor[i] == ',' && i < 4)) {
          setReply(result, "?");
          return result;
        }
        word[i] = cursor[i];
      }
      uint8_t bytes[2]{};
      uint8_t n = 0;
      if (!parseHexBytes(word, bytes, 2, n) || n != 2) {
        setReply(result, "?");
        return result;
      }
      ElmVehicleRequest& item = result.batch[result.batch_count++];
      item.can_id = header;
      item.length = uds ? 3 : 2;
      item.data[0] = uds ? 0x22 : bytes[0];
      item.data[1] = uds ? bytes[0] : bytes[1];
      if (uds) {
        item.data[2] = bytes[1];
      } else if (bytes[0] != 0x01 ||
                 !(bytes[1] == 0x0C || bytes[1] == 0x0D ||
                   bytes[1] == 0x05 || bytes[1] == 0x5B ||
                   bytes[1] == 0x9A)) {
        setReply(result, "?");
        return result;
      }
      cursor += 4;
      if (*cursor == ',') ++cursor;
      else if (*cursor) {
        setReply(result, "?");
        return result;
      }
      if (!*cursor && cursor[-1] == ',') {
        setReply(result, "?");
        return result;
      }
    }
    if (!result.batch_count) {
      setReply(result, "?");
      return result;
    }
    result.action = ElmAction::BatchRead;
    return result;
  }

  if (!std::strcmp(cmd, "ATM5TX1")) {
    result.action = ElmAction::LeaseAcquire;
    return result;
  }
  if (!std::strcmp(cmd, "ATM5TX0")) {
    result.action = ElmAction::LeaseRevoke;
    return result;
  }
  if (!std::strcmp(cmd, "ATM5STAT")) {
    result.action = ElmAction::LeaseStatus;
    return result;
  }
  if (!std::strcmp(cmd, "ATM5TEXT")) {
    result.action = ElmAction::ExitElmMode;
    setReply(result, "OK");
    return result;
  }

  if (startsWith(cmd, "AT")) {
    setReply(result, "?");
    return result;
  }

  uint8_t bytes[8]{};
  uint8_t length = 0;
  if (!parseHexBytes(cmd, bytes, sizeof(bytes), length) ||
      !validateVehicleRead(bytes, length)) {
    setReply(result, "?");
    return result;
  }

  result.action = ElmAction::VehicleRead;
  result.request.can_id = effectiveHeader();
  result.request.extended = true;
  result.request.length = length;
  std::memcpy(result.request.data, bytes, length);
  return result;
}

}  // namespace m5can
