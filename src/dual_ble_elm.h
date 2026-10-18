#pragma once
#include <Arduino.h>
#include <atomic>
#include "ble_elm_transport.h"
#include "can_monitor.h"
#include "elm_compat.h"
#include "demo_drive.h"
#include "adaptive_multi_pid.h"
#include "diagnostic_pid_cache.h"
#if defined(M5CAN_LOG_STREAM_V2_EXPERIMENTAL)
#include "m5can_log_stream_v2_sender.h"
#include "m5can_log_v2_progress.h"
#include "m5can_log_stream_v2_gatt.h"
#endif

namespace m5can {
class SerialProtocol;
#if defined(M5CAN_RAW_CAN_LOG_RUNTIME)
class RawCanRecordingService;
#endif

// Experimental two-client BLE ELM front-end. All state lives in this class,
// which is polled from the Arduino loop. CAN continues on the existing
// independent TWAI owner task, and at most one query is in flight.
class DualBleElm {
 public:
  DualBleElm(CanMonitor& can, BleElmTransport& ble,
             SerialProtocol& demo_source)
      : can_(can), ble_(ble), demo_source_(demo_source) {}
  void poll();
#if defined(M5CAN_RAW_CAN_LOG_RUNTIME)
  void setRawCanRecorder(RawCanRecordingService* service) { raw_service_=service; }
#endif
  struct CoordinatorStats {
    uint32_t cache_hits=0, cache_misses=0, joins=0;
    uint32_t grouped_submissions=0, cross_group_submissions=0;
    uint32_t single_submissions=0;
  };
  CoordinatorStats coordinatorStats() const {
    CoordinatorStats s{};
    s.cache_hits=cache_hits_.load(std::memory_order_relaxed);
    s.cache_misses=cache_misses_.load(std::memory_order_relaxed);
    s.joins=joins_.load(std::memory_order_relaxed);
    s.grouped_submissions=grouped_submissions_.load(std::memory_order_relaxed);
    s.cross_group_submissions=cross_groups_.load(std::memory_order_relaxed);
    s.single_submissions=single_submissions_.load(std::memory_order_relaxed);
    return s;
  }

 private:
  struct Session {
    bool active = false;
    uint16_t conn_id = 0;
    uint32_t generation = 0;
    ElmCompat elm{};
    char input[129]{};
    size_t input_size = 0;
    bool input_overflow = false;
    uint8_t output[512]{};
    size_t output_size = 0;
    bool pending = false;
    bool batch = false;
    bool multi_pid_pair = false;
    uint8_t batch_next = 0;
    uint8_t batch_count = 0;
    ElmVehicleRequest items[ElmResult::kBatchMaxIDs]{};
    DiagnosticRequest request{};
  };
  void event(const BleClientEvent& input);
  void onLine(Session& session, const char* line);
  void emit(Session& session, const char* text);
  void emitBytes(Session& session, const uint8_t* data, size_t length);
  void emitLine(Session& session, const char* text);
  void prompt(Session& session);
  void flush(Session& session);
  void appendFrame(Session& session, const DiagnosticRawFrame& frame);
  void appendResult(Session& session, const DiagnosticResult& result);
  void advanceBatch(Session& session);
  void dispatch();
  void onResult();
  void finishSingle(Session& session, const DiagnosticResult& result);
  void abortBatch(Session& session, const DiagnosticResult& result);
  void cacheSafePartialGroup(const DiagnosticResult& result,
                             const DiagnosticRequest& rpm,
                             const DiagnosticRequest& speed,
                             uint64_t timestamp_us);
  Session* find(uint16_t conn_id, uint32_t generation);
  static DiagnosticRequest requestOf(const ElmVehicleRequest& item,
                                     const ElmCompat& elm);

#if defined(M5CAN_LOG_STREAM_V2_EXPERIMENTAL) && defined(M5CAN_RAW_CAN_LOG_RUNTIME)
  void pollLogV2();
  m5can_log_stream_v2::BlockSender log_stream_sender_{};
  uint16_t log_stream_peer_=0;
  uint32_t log_stream_generation_=0;
  m5can_log_stream_v2::ProgressWatchdog log_stream_progress_{};
  bool log_stream_active_=false,log_stream_fetch_pending_=false;
#endif
  AdaptiveMultiPid adaptive_multi_pid_{};
  // Shared across both BLE sessions, independent of the CAN grouping policy.
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
  DiagnosticPidCache pid_cache_{};
  // Cache and its large output buffer do not consume RAM in the stable
  // dual-BLE profile; only the experimental grouped image owns them.
  DiagnosticResult cache_workspace_{};
#endif
  // Separately retained return buffer for the physical CAN transaction.
  // Never reserve the ~1.7 KiB DiagnosticResult on the BLE worker stack.
  DiagnosticResult query_result_workspace_{};
  CanMonitor& can_;
  BleElmTransport& ble_;
  SerialProtocol& demo_source_;
#if defined(M5CAN_RAW_CAN_LOG_RUNTIME)
  RawCanRecordingService* raw_service_=nullptr;
#endif
  Session sessions_[2]{};
  // Atomics are published from BLE core1 and read by the Arduino/UI
  // task without exposing non-thread-safe cache internals.
  std::atomic<uint32_t> cache_hits_{0},cache_misses_{0},joins_{0};
  std::atomic<uint32_t> grouped_submissions_{0},cross_groups_{0};
  std::atomic<uint32_t> single_submissions_{0};
  int active_query_owner_ = -1;
  // A second pending request may join the same physical CAN transaction.
  int active_secondary_owner_ = -1;
  uint32_t active_secondary_generation_ = 0;
  bool active_cross_group_ = false;
  uint32_t active_query_generation_ = 0;
  uint8_t round_robin_ = 0;
};
} // namespace m5can
