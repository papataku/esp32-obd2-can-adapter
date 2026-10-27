#pragma once
// V2-1b deterministic BENCH ONLY synthetic OBD payloads. Never a CAN source.
#include <cstddef>
#include <cstdint>
namespace signal_v2 {
inline size_t demoPayload(uint16_t pid, uint32_t sequence, uint8_t *out) {
  if (!out) return 0;
  out[0] = 0x41;
  out[1] = static_cast<uint8_t>(pid);
  switch (pid) {
  case 0x0c: {
    const uint16_t rpm = static_cast<uint16_t>(850 + (sequence * 79U) % 3900U);
    const uint16_t raw = rpm * 4U;
    out[2] = raw >> 8; out[3] = static_cast<uint8_t>(raw);
    return 4;
  }
  case 0x0d:
    out[2] = static_cast<uint8_t>((sequence * 7U) % 121U);
    return 3;
  case 0x05:
    out[2] = static_cast<uint8_t>(40 + 78 + (sequence % 17U));
    return 3;
  case 0x5b:
    out[2] = static_cast<uint8_t>(102 + (sequence * 3U) % 78U);
    return 3;
  case 0x9a: {
    // RP8-compatible 41 9A xx xx V_hi V_lo I_hi I_lo.
    // Derived kW = (V/64)*(signed I/10)/1000. Synthetic ONLY.
    const uint16_t voltage = static_cast<uint16_t>((300 + sequence % 24U) * 64U);
    const int16_t current = static_cast<int16_t>((int32_t(sequence % 81U) - 40) * 10);
    out[2] = out[3] = 0;
    out[4] = voltage >> 8; out[5] = static_cast<uint8_t>(voltage);
    out[6] = static_cast<uint16_t>(current) >> 8;
    out[7] = static_cast<uint8_t>(current);
    return 8;
  }
  default: return 0;
  }
}
} // namespace signal_v2
