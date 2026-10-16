#pragma once
#include "can_monitor.h"
#include "raw_can_codec.h"
#include "raw_can_partition.h"
#include <atomic>

namespace m5can {

// CPU1/Arduino single-owner service. CAN owner only touches RawCanCapture.
// Flash is written solely while the shared CAN-idle checkpoint is held.
class RawCanRecordingService {
 public:
  enum class State : uint8_t {
    Unavailable=0, Idle=1, Recording=2, Stopping=3, Complete=4,
    Fault=5, RecoveryRequired=6,
  };
  // BLE worker posts a request; Arduino owner performs flash/session changes.
  // The same peer generation must submit STOP. This is peer isolation, not
  // cryptographic authentication; raw data export needs secure pairing.
  bool queueStartFromBle(uint32_t session,uint16_t peer,uint32_t generation);
  bool queueStopFromBle(uint16_t peer,uint32_t generation);
  State state() const { return state_.load(std::memory_order_acquire); }
  uint32_t lastControlError() const {
    return last_control_error_.load(std::memory_order_relaxed);
  }
  explicit RawCanRecordingService(CanMonitor& can) : can_(can) {}

  // Initialize/scan while safe on boot, before CAN and BLE workers start.
  bool begin();
  // USB bench API for now. Future BLE control plane must enqueue requests to
  // this same Arduino owner; never call start/stop/poll in GAP callbacks.
  bool start(uint32_t session_id);
  bool stop();
  void poll();
  bool ready() const { return rawCanJournal().status().ready; }
  bool recording() const { return recording_; }
  bool stopping() const { return stopping_; }
  bool fault() const { return fault_; }
  uint32_t session() const { return session_id_.load(std::memory_order_acquire); }
  uint32_t pending() const { return can_.rawCapture().queued(); }
  uint32_t dropped() const { return can_.rawCapture().lost(); }
  uint32_t committedBlocks() const { return rawCanJournal().status().blocks; }
  uint32_t committedBytes() const { return rawCanJournal().status().bytes; }
  uint32_t storageErrors() const { return rawCanJournal().status().errors; }

 private:
  bool commit();
  void fail();
  CanMonitor& can_;
  RawCanBlockCodec encoder_{};
  RawCanFrame pending_frame_{};
  std::atomic<uint32_t> session_id_{0};
  std::atomic<State> state_{State::Unavailable};
  std::atomic<uint32_t> requested_session_{0};
  std::atomic<uint16_t> requested_peer_{0}, owner_peer_{0};
  std::atomic<uint32_t> requested_generation_{0}, owner_generation_{0};
  std::atomic<bool> requested_stop_{false};
  std::atomic<uint32_t> last_control_error_{0};
  uint32_t last_flush_ms_=0;
  bool has_pending_frame_=false;
  bool recording_=false,stopping_=false,fault_=false;
};

} // namespace m5can
