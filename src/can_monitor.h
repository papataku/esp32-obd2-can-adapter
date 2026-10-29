#pragma once

#include <Arduino.h>
#include <driver/twai.h>

#include "diagnostic_policy.h"
#include "diagnostic_request_pacer.h"
#include "diagnostic_result_cache.h"
#include "diagnostic_transaction.h"
#include "raw_can_capture.h"
#include "signal_v2_can_budget.h"

namespace m5can {

struct CapturedFrame {
  uint64_t timestamp_us = 0;
  uint32_t identifier = 0;
  uint8_t dlc = 0;
  bool extended = false;
  bool rtr = false;
  bool self = false;
  uint8_t data[8]{};
};

struct CanStats {
  uint64_t signal_v2_rx = 0, signal_v2_fc = 0; // Actual candidate RX / successful FC submissions.
  uint32_t observed100=0,observed1000=0,observed_peak100=0,observed_peak1000=0;
  uint64_t rx_frames = 0;
  uint64_t std_frames = 0;
  uint64_t std_obd_responses = 0;
  uint64_t ext_frames = 0;
  uint64_t rtr_frames = 0;
  uint64_t app_queue_drops = 0;
  uint64_t tx_attempts = 0;
  uint64_t tx_success = 0;
  uint64_t query_count = 0;
  uint64_t query_timeout = 0;
  uint64_t query_ok = 0;
  uint64_t query_cache_hits = 0;        // Positive validated ISO-TP response
  uint64_t query_no_data = 0;   // No matching response
  uint64_t query_failed = 0;    // Lease, TX, bus or protocol error
  // CAN owner classifications, including USB-originated requests that
  // never pass through DualBleElm. Index is DiagnosticStatus raw enum.
  uint32_t result_status_count[10]{};
  uint32_t last_failed_request=0,last_failed_ms=0;
  uint32_t nrc_count=0,first_nrc_detail=0,last_nrc_detail=0;
  uint32_t first_nrc_request=0,last_nrc_request=0,first_nrc_ms=0,last_nrc_ms=0;
  uint32_t last_response_can_id = 0; // Actual responder, not ambient CAN RX
  bool query_active = false;
  uint32_t driver_rx_missed = 0;
  uint32_t driver_rx_overrun = 0;
  uint32_t driver_bus_error = 0;
  uint32_t driver_arb_lost = 0;
  uint32_t rx_queue_depth = 0;
  twai_state_t state = TWAI_STATE_STOPPED;
};

class CanMonitor {
 public:
  bool begin();
  bool pop(CapturedFrame& frame, TickType_t wait_ticks = 0);
  CanStats stats() const;
  // Fill caller-owned storage without a full snapshot temporary on its stack.
  void stats(CanStats& copy) const;
  RawCanCapture& rawCapture() { return raw_capture_; }
  const RawCanCapture& rawCapture() const { return raw_capture_; }
  bool running() const { return running_; }
  const char* lastError() const { return last_error_; }
  bool faultLocked() const { return fault_locked_; }

  bool acquireLease(uint32_t duration_ms);
  void revokeLease();
  uint32_t leaseRemainingMs() const;

  uint32_t ownerStackFreeBytes() const {
    return rx_task_ ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(rx_task_)) : 0;
  }
  // Share the same reservation as submitQuery: NVS writes must not overlap
  // an active diagnostic transaction. Do not hold a spinlock during flash IO.
  bool tryBeginLogCheckpoint() {
    portENTER_CRITICAL(&query_mux_);
    const bool accepted=!query_in_flight_ && !log_checkpoint_active_;
    if(accepted)log_checkpoint_active_=true;
    portEXIT_CRITICAL(&query_mux_);
    return accepted;
  }
  void endLogCheckpoint() {
    portENTER_CRITICAL(&query_mux_);
    log_checkpoint_active_=false;
    portEXIT_CRITICAL(&query_mux_);
  }
  bool submitQuery(const DiagnosticRequest& request);
  bool pollQueryResult(DiagnosticResult& result);
  bool queryPending() const { return query_in_flight_; }

  bool query(const DiagnosticRequest& request, DiagnosticResult& result,
             TickType_t wait_ticks);

 private:
  static void rxTaskThunk(void* arg);
  void rxTask();

  bool installDriver(twai_mode_t mode);
  bool switchDriverMode(twai_mode_t mode);
  CapturedFrame captureMessage(const twai_message_t& message,
                               uint64_t timestamp_us);
  void maybeUpdateDriverStats(uint64_t now_us);
  void updateDriverStats();
  void setError(const char* message);

  bool recordObservedDiagnostic(uint32_t now);
  bool leaseAllowsBudget(uint32_t budget_ms) const;
  bool waitForRateLimit(const DiagnosticRequest& request);
  void performQuery(const DiagnosticRequest& request, DiagnosticResult& result);

  esp_err_t transmitOwnedFrame(const twai_message_t& message,
                               uint32_t wait_ms);
  static twai_message_t makeDiagnosticRequestFrame(
      const DiagnosticRequest& request);
  static twai_message_t makeFlowControlFrame(uint32_t can_id);

  void enterFaultLocked(const char* reason);

  QueueHandle_t frame_queue_ = nullptr;
  QueueHandle_t query_queue_ = nullptr;
  QueueHandle_t result_queue_ = nullptr;
  TaskHandle_t rx_task_ = nullptr;

  mutable portMUX_TYPE query_mux_ = portMUX_INITIALIZER_UNLOCKED;
  mutable portMUX_TYPE stats_mux_ = portMUX_INITIALIZER_UNLOCKED;
  mutable portMUX_TYPE lease_mux_ = portMUX_INITIALIZER_UNLOCKED;
  CanStats stats_{};
  RawCanCapture raw_capture_{}; // CAN-owner producer, Arduino writer consumer
  volatile bool running_ = false;
  volatile bool fault_locked_ = false;
  volatile bool query_in_flight_ = false;
  bool log_checkpoint_active_ = false;
  uint64_t last_status_poll_us_ = 0;
  DiagnosticRequestPacer diagnostic_pacer_{};
  DiagnosticResultCache result_cache_{};
  // Only rxTask accesses these. Queues/cache copy the completed result before
  // reuse; avoid multi-kilobyte result/transaction frames around TWAI IPC calls.
  DiagnosticResult owner_result_{};
  DiagnosticTransaction owner_transaction_{};
  mutable signal_v2::CanBudget observed_budget_{}; // protected by stats_mux_
  signal_v2::CanBudget signal_budget_{}; // CAN owner task only. Never reset at ARM.
  uint64_t lease_deadline_ms_ = 0;
  char last_error_[96] = "not started";
};

}  // namespace m5can
