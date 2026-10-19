#pragma once
#include "can_monitor.h"
#include "raw_can_codec.h"
#include "raw_can_ble_protocol.h"
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
  uint32_t dropped() const {
    const uint32_t live=can_.rawCapture().lost();
    const uint32_t saved=retained_dropped_.load(std::memory_order_acquire);
    return live>saved?live:saved;
  }
  uint32_t committedBlocks() const { return rawCanJournal().status().blocks; }
  uint32_t committedBytes() const { return rawCanJournal().status().bytes; }
  uint32_t storageErrors() const { return rawCanJournal().status().errors; }

  // One outstanding bounded read. BLE core queues; Arduino core owns flash
  // reads and replies with a CRC32 per chunk. No RAW data is read in GAP/GATT.
  // Competing requests fail closed; client retries after receiving a reply.
  struct ExportReply {
    uint16_t peer=0;
    uint32_t generation=0, index=0, offset=0;
    uint16_t total=0;
    uint8_t type=0, count=0, error=0;
    uint32_t crc=0;
    uint8_t bytes[raw_ble::kChunkBytes]{};
  };
  bool queueReadFromBle(uint32_t index,uint32_t offset,
                        uint16_t peer,uint32_t generation);
  bool takeReadForBle(ExportReply& reply);

#if defined(M5CAN_LOG_STREAM_V2_EXPERIMENTAL)
  // Isolated bench stage for V2: transfer ONE full committed MCAN journal
  // block at once. No flash I/O from BLE task/callback and no journal erase.
  // External GATT ownership and authenticated log subscriber are NOT wired.
  struct StreamBlockReply {
    uint16_t peer=0,total=0;
    uint32_t generation=0,index=0;
    uint8_t type=0,error=0;
    uint8_t bytes[RawCanBlockCodec::kMaxBytes]{};
  };
  bool queueStreamBlockFromBle(uint32_t index,uint16_t peer,uint32_t generation);
  bool takeStreamBlockForBle(StreamBlockReply& reply);
#endif

 private:
  bool commit();
  void fail();
  CanMonitor& can_;
  RawCanBlockCodec encoder_{};
  RawCanFrame pending_frame_{};
  std::atomic<uint32_t> session_id_{0};
  std::atomic<uint32_t> retained_dropped_{0};
  std::atomic<State> state_{State::Unavailable};
  std::atomic<uint32_t> requested_session_{0};
  std::atomic<uint16_t> requested_peer_{0}, owner_peer_{0};
  std::atomic<uint32_t> requested_generation_{0}, owner_generation_{0};
  std::atomic<bool> requested_stop_{false};
  std::atomic<uint32_t> last_control_error_{0};
  uint32_t last_flush_ms_=0;
  // BLE request owner core1 -> Arduino owner core1 loop. The state machine
  // is a release/acquire mailbox and prevents a torn result on reconnect.
  enum : uint8_t { ReadIdle=0, ReadFilling=1, ReadQueued=2,
                   ReadProcessing=3, ReadReady=4 };
  std::atomic<uint8_t> read_state_{ReadIdle};
  ExportReply read_reply_{};
  uint8_t read_buffer_[RawCanBlockCodec::kMaxBytes]{};
  void pollRead();
#if defined(M5CAN_LOG_STREAM_V2_EXPERIMENTAL)
  void pollStreamRead();
  // Mailbox transfers a verified journal block from Arduino flash owner
  // to BLE app; copy is bounded 3616B, independent from legacy 48B reads.
  std::atomic<uint8_t> stream_state_{ReadIdle};
  StreamBlockReply stream_reply_{};
#endif
  bool has_pending_frame_=false;
  bool recording_=false,stopping_=false,fault_=false;
};

} // namespace m5can
