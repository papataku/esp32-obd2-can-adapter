#pragma once
#include <array>
#include <cstdint>
namespace signal_v2 {
// Single BLE application owner, shared across both sessions. The source never
// touches CAN. Session demand expires synchronously before this scheduler runs.
class Simulator {
public:
  static constexpr uint16_t Pids[5] = {0x0d, 0x0c, 0x05, 0x5b, 0x9a};
  struct Sample {
    uint32_t observed_ms = 0, sequence = 0, next_ms = 0;
    bool valid = false;
  };
  using Demand = std::array<uint16_t, 5>;

private:
  std::array<Sample, 5> samples_{};
  uint32_t acquisitions_ = 0;
  uint8_t reservations_ = 0;

public:
  void tick(uint32_t now, const Demand &demand) {
    reservations_ = 0;
    for (unsigned i = 0; i < 5; ++i) {
      auto &s = samples_[i];
      if (!demand[i]) {
        s.valid = false;
        continue;
      }
      ++reservations_;
      if (!s.valid || int32_t(now - s.next_ms) >= 0) {
        s.valid = true;
        s.observed_ms = now;
        s.next_ms = now + demand[i];
        ++s.sequence;
        ++acquisitions_;
      }
    }
  }
  const Sample *sample(uint16_t pid) const {
    for (unsigned i = 0; i < 5; ++i)
      if (Pids[i] == pid)
        return samples_[i].valid ? &samples_[i] : nullptr;
    return nullptr;
  }
  uint32_t acquisitions() const { return acquisitions_; }
  uint8_t reservations() const { return reservations_; }
};
} // namespace signal_v2
