// Deterministic malformed-frame replay. No CAN TX or TWAI hardware.
#if !defined(M5CAN_MULTI_ECU_POLICY_DRY_BENCH) || !defined(M5CAN_CAN_MODE_DRY_RUN)
#error "Replay must only run with experimental physical CAN TX disabled"
#endif
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include "diagnostic_transaction.h"

using namespace m5can;

static uint32_t rng = 0x9E3779B9U;
static uint32_t draw() {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return rng;
}

int main() {
  // A deterministic seed makes all failures reproducible in GitHub Actions.
  constexpr uint32_t ids[] = {
    0x18DAF102U, 0x18DAF106U, 0x18DAF10EU,
    0x18DAF1EFU, 0x18DAF101U, 0x18DAF180U, 0x7E8U
  };
  constexpr uint32_t physical = 0x18DA02F1U;
  constexpr uint32_t expected = 0x18DAF102U;
  DiagnosticRequest request{};
  request.can_id = physical;
  request.length = 3;
  request.data[0] = 0x22;
  request.data[1] = 0x20;
  request.data[2] = 0x12;

  DiagnosticTransaction transaction;
  constexpr int kCases = 16000;
  for (int iteration = 0; iteration < kCases; ++iteration) {
    const uint64_t start = 10'000'000ULL +
        static_cast<uint64_t>(iteration) * 2'000'000ULL;
    transaction.begin(request, start, 100);

    for (int frameIndex = 0; frameIndex < 4; ++frameIndex) {
      TransactionFrame f{};
      f.can_id = ids[draw() % (sizeof(ids) / sizeof(ids[0]))];
      f.extended = (draw() % 5) != 0;
      f.rtr = (draw() % 7) == 0;
      f.timestamp_us = start + (draw() % 150000U);
      if ((draw() & 7U) == 0) f.timestamp_us = start - 1;
      const uint32_t lenCase = draw() % 20;
      f.dlc = static_cast<uint8_t>(lenCase == 19 ? 255 : lenCase);
      for (auto& value : f.data) value = static_cast<uint8_t>(draw());
      // Periodically force an otherwise correctly routed FF, but scramble
      // sequence/timing/foreign ID around it.
      if (frameIndex == 0 && iteration % 3 == 0) {
        f.can_id = expected;
        f.extended = true;
        f.rtr = false;
        f.dlc = 8;
        const uint8_t ff[] = {0x10, 0x0C, 0x62, 0x20, 0x12, 1, 2, 3};
        std::memcpy(f.data, ff, sizeof(ff));
        f.timestamp_us = start + 1;
      }
      const auto step = transaction.onFrame(f);
      if (step.event == TransactionEvent::NeedFlowControl) {
        assert(f.can_id == expected);
        assert(step.flow_control_can_id == physical);
      } else {
        assert(step.flow_control_can_id == 0);
      }
      const auto& result = transaction.result();
      assert(result.raw_frame_count <= kDiagnosticMaxRawFrames);
      assert(result.response_count <= kDiagnosticMaxResponses);
      for (unsigned i = 0; i < result.response_count; ++i) {
        assert(result.responses[i].can_id == expected);
        assert(result.responses[i].length >= 3);
        assert(result.responses[i].length <= kDiagnosticMaxPayload);
        if (result.responses[i].data[0] == 0x62) {
          assert(result.responses[i].data[1] == 0x20);
          assert(result.responses[i].data[2] == 0x12);
        } else {
          assert(result.responses[i].data[0] == 0x7F);
          assert(result.responses[i].data[1] == 0x22);
        }
      }
    }
    transaction.expireIsoTp(start + 250000);
    assert(!transaction.hasActiveIsoTp());
  }
  std::cout << "PASS seeded 16000-case malformed ISO-TP/ECU replay (TX disabled)\n";
  return 0;
}
