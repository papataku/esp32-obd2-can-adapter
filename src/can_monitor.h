#pragma once

#include <Arduino.h>
#include <driver/twai.h>

#include "diagnostic_policy.h"

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
  uint64_t rx_frames = 0;
  uint64_t std_frames = 0;
  uint64_t ext_frames = 0;
  uint64_t rtr_frames = 0;
  uint64_t app_queue_drops = 0;
  uint64_t tx_attempts = 0;
  uint64_t tx_success = 0;
  uint64_t query_count = 0;
  uint64_t query_timeout = 0;
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
  bool running() const { return running_; }
  const char* lastError() const { return last_error_; }

  bool acquireLease(uint32_t duration_ms);
  void revokeLease();
  uint32_t leaseRemainingMs() const;
  bool query(const DiagnosticRequest& request, DiagnosticResult& result,
             TickType_t wait_ticks);

 private:
  static void rxTaskThunk(void* arg);
  void rxTask();

  bool installDriver(twai_mode_t mode);
  bool switchDriverMode(twai_mode_t mode);
  CapturedFrame captureMessage(const twai_message_t& message, uint64_t timestamp_us);
  void maybeUpdateDriverStats(uint64_t now_us);
  void updateDriverStats();
  void setError(const char* message);

  bool leaseAllowsBudget(uint32_t budget_ms) const;
  bool waitForRateLimit();
  DiagnosticResult performQuery(const DiagnosticRequest& request);
  bool appendSingleFrameResponse(const DiagnosticRequest& request,
                                 const CapturedFrame& frame,
                                 DiagnosticResult& result,
                                 bool& multi_frame_match);
  void enterFaultLocked(const char* reason);

  QueueHandle_t frame_queue_ = nullptr;
  QueueHandle_t query_queue_ = nullptr;
  QueueHandle_t result_queue_ = nullptr;
  TaskHandle_t rx_task_ = nullptr;

  mutable portMUX_TYPE stats_mux_ = portMUX_INITIALIZER_UNLOCKED;
  mutable portMUX_TYPE lease_mux_ = portMUX_INITIALIZER_UNLOCKED;
  CanStats stats_{};
  volatile bool running_ = false;
  volatile bool fault_locked_ = false;
  uint64_t last_status_poll_us_ = 0;
  uint64_t last_tx_us_ = 0;
  uint64_t lease_deadline_ms_ = 0;
  char last_error_[96] = "not started";
};

}  // namespace m5can
