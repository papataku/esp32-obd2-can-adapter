#include "native_protocol.h"

#include <cstring>

namespace m5can::native {
namespace {

constexpr size_t kHeaderSize = 7;
constexpr size_t kCrcSize = 4;

void putU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void putU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void putU64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint16_t getU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1U)));
      crc = (crc >> 1) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

size_t cobsEncode(const uint8_t* input, size_t length, uint8_t* output, size_t capacity) {
  if (!output || capacity == 0) return 0;
  size_t read = 0, write = 1, code_index = 0;
  uint8_t code = 1;
  while (read < length) {
    if (write >= capacity) return 0;
    if (input[read] == 0) {
      output[code_index] = code;
      code = 1;
      code_index = write++;
      ++read;
    } else {
      output[write++] = input[read++];
      if (++code == 0xFF) {
        output[code_index] = code;
        code = 1;
        code_index = write++;
        if (code_index >= capacity) return 0;
      }
    }
  }
  output[code_index] = code;
  return write;
}

size_t cobsDecode(const uint8_t* input, size_t length, uint8_t* output, size_t capacity) {
  if (!input || !output || length == 0) return 0;
  size_t read = 0, write = 0;
  while (read < length) {
    const uint8_t code = input[read++];
    if (code == 0) return 0;
    for (uint8_t i = 1; i < code; ++i) {
      if (read >= length || write >= capacity) return 0;
      output[write++] = input[read++];
    }
    if (code != 0xFF && read < length) {
      if (write >= capacity) return 0;
      output[write++] = 0;
    }
  }
  return write;
}

size_t encodePacket(PacketType type, uint8_t flags, uint16_t sequence,
                    const uint8_t* payload, uint16_t payload_len,
                    uint8_t* output, size_t output_capacity) {
  if (payload_len > cfg::kNativeMaxPayload || (payload_len && !payload)) return 0;
  uint8_t decoded[cfg::kNativeMaxDecodedPacket]{};
  const size_t decoded_len = kHeaderSize + payload_len + kCrcSize;
  if (decoded_len > sizeof(decoded) || output_capacity < 2) return 0;

  decoded[0] = cfg::kNativeProtocolVersion;
  decoded[1] = static_cast<uint8_t>(type);
  decoded[2] = flags;
  putU16(&decoded[3], sequence);
  putU16(&decoded[5], payload_len);
  if (payload_len) std::memcpy(&decoded[kHeaderSize], payload, payload_len);
  putU32(&decoded[kHeaderSize + payload_len], crc32(decoded, kHeaderSize + payload_len));

  const size_t encoded_len = cobsEncode(decoded, decoded_len, output, output_capacity - 1);
  if (!encoded_len || encoded_len + 1 > output_capacity) return 0;
  output[encoded_len] = 0;
  return encoded_len + 1;
}

bool decodePacket(const uint8_t* encoded, size_t encoded_len, DecodedPacket& out) {
  uint8_t decoded[cfg::kNativeMaxDecodedPacket]{};
  const size_t decoded_len = cobsDecode(encoded, encoded_len, decoded, sizeof(decoded));
  if (decoded_len < kHeaderSize + kCrcSize || decoded[0] != cfg::kNativeProtocolVersion) return false;
  const uint16_t payload_len = getU16(&decoded[5]);
  if (payload_len > cfg::kNativeMaxPayload) return false;
  if (decoded_len != kHeaderSize + payload_len + kCrcSize) return false;
  if (getU32(&decoded[kHeaderSize + payload_len]) != crc32(decoded, kHeaderSize + payload_len)) return false;

  out.type = static_cast<PacketType>(decoded[1]);
  out.flags = decoded[2];
  out.sequence = getU16(&decoded[3]);
  out.payload_len = payload_len;
  if (payload_len) std::memcpy(out.payload, &decoded[kHeaderSize], payload_len);
  return true;
}

size_t buildCanFramePayload(const CapturedFrame& frame, uint8_t* output, size_t capacity) {
  const size_t data_len = frame.rtr ? 0 : frame.dlc;
  const size_t required = 14 + data_len;
  if (!output || frame.dlc > 8 || capacity < required) return 0;
  putU64(&output[0], frame.timestamp_us);
  putU32(&output[8], frame.identifier);
  output[12] = frame.dlc;
  output[13] = (frame.extended ? 0x01 : 0) |
               (frame.rtr ? 0x02 : 0) |
               (frame.self ? 0x04 : 0);
  if (data_len) std::memcpy(&output[14], frame.data, data_len);
  return required;
}

}  // namespace m5can::native
