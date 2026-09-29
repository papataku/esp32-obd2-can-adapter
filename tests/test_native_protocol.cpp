#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "native_protocol.h"

using namespace m5can;
using namespace m5can::native;

int main() {
  const uint8_t ref[] = {'1','2','3','4','5','6','7','8','9'};
  assert(crc32(ref, sizeof(ref)) == 0xCBF43926U);

  const uint8_t payload[] = {0x11,0x00,0x22,0x33,0x00,0x44};
  uint8_t wire[cfg::kNativeMaxEncodedPacket]{};
  size_t n = encodePacket(PacketType::Event, 3, 0x1234, payload, sizeof(payload),
                          wire, sizeof(wire));
  assert(n > 1 && wire[n - 1] == 0);
  DecodedPacket decoded{};
  assert(decodePacket(wire, n - 1, decoded));
  assert(decoded.type == PacketType::Event);
  assert(decoded.flags == 3 && decoded.sequence == 0x1234);
  assert(decoded.payload_len == sizeof(payload));
  assert(std::memcmp(decoded.payload, payload, sizeof(payload)) == 0);

  CapturedFrame frame{};
  frame.timestamp_us = 123456;
  frame.identifier = 0x7E8;
  frame.dlc = 4;
  frame.data[0]=0x41; frame.data[1]=0x0C; frame.data[2]=0x13; frame.data[3]=0x88;
  uint8_t can_payload[32]{};
  assert(buildCanFramePayload(frame, can_payload, sizeof(can_payload)) == 18);
  assert(can_payload[12] == 4 && can_payload[13] == 0);
  assert(can_payload[14] == 0x41 && can_payload[17] == 0x88);

  std::vector<uint8_t> max_payload(cfg::kNativeMaxPayload, 0xA5);
  n = encodePacket(PacketType::Event, 0, 9, max_payload.data(),
                   static_cast<uint16_t>(max_payload.size()), wire, sizeof(wire));
  assert(n > 0 && decodePacket(wire, n - 1, decoded));
  assert(decoded.payload_len == cfg::kNativeMaxPayload);

  std::cout << "PASS C++ native protocol tests\n";
  return 0;
}
