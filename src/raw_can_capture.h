#pragma once
// CAN owner (CPU0) -> journal worker (Arduino loop) SPSC, allocation-free.
// Production firmware always compiles this. A session starts ONLY on explicit
// START; there is no compile-time user-facing CAN logging OFF switch.
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace m5can {

enum class RawCanDirection : uint8_t {
  Rx = 0, TxSubmitted = 1, TxSubmitFailed = 2,
};

// Timestamps are ESP timer observations, not hardware timestamping of the bus.
// TxSubmitted means TWAI accepted submission, not confirmed bus arbitration.
struct RawCanFrame {
  uint64_t timestamp_us = 0;
  uint32_t sequence = 0;
  uint32_t identifier = 0;
  uint8_t data[8]{};
  uint8_t dlc = 0;
  uint8_t flags = 0;  // bit0 extended, bit1 RTR, bit2 self
  RawCanDirection direction = RawCanDirection::Rx;
};
static_assert(sizeof(RawCanFrame) <= 32, "raw frame must be bounded");

class RawCanCapture {
 public:
  static constexpr uint32_t kCapacity = 256; // ~8 KiB, fixed at compile-time

  // Caller: management worker only. May start only after all old buffered
  // frames have been drained into a durable session, never silently discard.
  bool start(uint32_t session_id) {
    if (!session_id || active_.load(std::memory_order_acquire) ||
        head_.load(std::memory_order_acquire) !=
        tail_.load(std::memory_order_acquire)) return false;
    sequence_ = 0;
    attempted_.store(0, std::memory_order_relaxed);
    lost_.store(0, std::memory_order_relaxed);
    session_id_.store(session_id, std::memory_order_release);
    active_.store(true, std::memory_order_release);
    return true;
  }
  void stop() { active_.store(false, std::memory_order_release); }
  bool active() const { return active_.load(std::memory_order_acquire); }
  uint32_t session() const { return session_id_.load(std::memory_order_acquire); }
  uint32_t attempted() const { return attempted_.load(std::memory_order_relaxed); }
  uint32_t lost() const { return lost_.load(std::memory_order_relaxed); }
  uint32_t queued() const {
    return head_.load(std::memory_order_acquire) -
           tail_.load(std::memory_order_acquire);
  }

  // CPU0 CAN owner is the ONLY producer (including TX outcome hook).
  // Never wait, allocate, format strings or write flash on this path.
  bool push(RawCanFrame frame) {
    if (!active_.load(std::memory_order_acquire)) return false;
    frame.sequence = ++sequence_;
    attempted_.fetch_add(1, std::memory_order_relaxed);
    const uint32_t head = head_.load(std::memory_order_relaxed);
    const uint32_t tail = tail_.load(std::memory_order_acquire);
    if (head - tail >= kCapacity) {
      lost_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    slots_[head % kCapacity] = frame;
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Arduino journal worker is the ONLY consumer.
  bool pop(RawCanFrame& frame) {
    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) return false;
    frame = slots_[tail % kCapacity];
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

 private:
  RawCanFrame slots_[kCapacity]{};
  std::atomic<uint32_t> head_{0}, tail_{0}, lost_{0}, attempted_{0};
  std::atomic<uint32_t> session_id_{0};
  std::atomic<bool> active_{false};
  uint32_t sequence_ = 0; // CPU0 owner only
};

} // namespace m5can
