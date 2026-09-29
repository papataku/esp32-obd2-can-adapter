#pragma once

#include <Arduino.h>
#include "app_config.h"
#include "can_monitor.h"

namespace m5can::native {

enum class PacketType : uint8_t {
  Hello = 0x01,
  Stats = 0x02,
  CanFrame = 0x03,
  Event = 0x04,
  Error = 0x05,
  Command = 0x10,
  CommandReply = 0x11,
};

enum class Command : uint8_t {
  Ping = 0x01,
  GetInfo = 0x02,
  GetStats = 0x03,
  StartStream = 0x04,
  StopStream = 0x05,
  TextMode = 0x06,
};

enum class Status : uint8_t {
  Ok = 0x00,
  BadCommand = 0x01,
  BadPayload = 0x02,
  BadPacket = 0x03,
};

struct DecodedPacket {
  PacketType type = PacketType::Error;
  uint8_t flags = 0;
  uint16_t sequence = 0;
  uint16_t payload_len = 0;
  uint8_t payload[cfg::kNativeMaxPayload]{};
};

uint32_t crc32(const uint8_t* data, size_t len);
size_t cobsEncode(const uint8_t* input, size_t length, uint8_t* output, size_t capacity);
size_t cobsDecode(const uint8_t* input, size_t length, uint8_t* output, size_t capacity);
size_t encodePacket(PacketType type, uint8_t flags, uint16_t sequence,
                    const uint8_t* payload, uint16_t payload_len,
                    uint8_t* output, size_t output_capacity);
bool decodePacket(const uint8_t* encoded, size_t encoded_len, DecodedPacket& out);
size_t buildCanFramePayload(const CapturedFrame& frame, uint8_t* output, size_t capacity);

}  // namespace m5can::native
