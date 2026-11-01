#pragma once
#include "signal_v2_can_route.h"
#include <cstring>
namespace signal_v2 { namespace profiles {
// Reviewed built-in data, not vehicle identity, binding or TX authorization.
// P1 schema naming: TacoTacoMeter cc8caca63db99e51dde7a5518c9a939d29a5ec6a.
struct Profile {
  const char* id;
  uint16_t schema;
  uint32_t revision;
  CanIdWidth width;
  uint32_t responder, request_id;
  uint8_t candidate_count;
  uint16_t pids[5];
  uint8_t probe_request[3], group_request[6];
  uint16_t deadline_ms, probe_period_ms, period_5hz_ms, period_10hz_ms;
  uint16_t probe_frames, group_frames, essential_frames, headroom_frames;
  uint32_t bits_per_frame, bits_100ms, bits_1s;
  const char* decision;
  const char* evidence_ref;
  const char* evidence_sha256;
};
// Internal linkage constexpr aggregate works with firmware C++11 and host tests.
constexpr Profile kLegacyRp8{
  "legacy-rp8", 1, 1, CanIdWidth::Extended29, 0x18DAF101U, 0x18DA01F1U, 1,
  {0x0D,0x0C,0x05,0x5B,0x9A}, {1,0x0C,0x0D}, {1,0x0D,0x0C,0x05,0x5B,0x9A},
  120,200,200,100, 4,12,4,16, 200,5000,25000,
  "24fbd7d8308729ecfa69cae3cdc3fd402af1cff0:O-5/O-6",
  "de635e9f628d46e26050019c15cf337482789276:include/signal_v2_discovery_profile.h",
  "dcdd603377522f4e4ff0e36821faacc5ebfbe657124235916b64cf79451b9b0d"
};
inline const Profile* builtin(const char* id) {
  return id && std::strcmp(id,kLegacyRp8.id)==0 ? &kLegacyRp8 : nullptr;
}
inline uint8_t candidateCount(const char* id) {
  return builtin(id) ? kLegacyRp8.candidate_count : 0;
}
} }
