#pragma once

#include <Arduino.h>
#include <driver/twai.h>
#include <freertos/semphr.h>

#include "diagnostic_policy.h"
#include "diagnostic_request_pacer.h"
#include "diagnostic_result_cache.h"
#include "diagnostic_transaction.h"
#include "raw_can_capture.h"
#include "signal_v2_can_budget.h"

namespace signal_v2 { struct CanOwnerTestAccess; } // host fixtures only
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

// PR-C2 stage S1: per-ID / per-ISO-TP-type count of every received frame
// during one bounded observation. Fixed capacity; overflow marks the audit
// incomplete instead of dropping silently.
struct ObservationAudit {
  static constexpr unsigned kIds = 8;
  enum Type : uint8_t { SF, FF, CF, FC, Other, kTypes };
  bool active = false;
  uint32_t ids[kIds]{};
  uint8_t extended[kIds]{};
  uint32_t counts[kIds][kTypes]{};
  uint8_t used = 0;
  uint32_t all_rx = 0, overflow = 0;
  uint32_t rx_inflight_at_end = 0; // Incomplete RX processing invalidates S1 evidence.
  // M-06: explicit receive blind interval while TWAI is reinstalled.
  uint32_t blind_count = 0;
  uint64_t blind_start_us = 0, blind_end_us = 0, blind_total_us = 0;
  uint64_t blind_open_since_us = 0;
  bool blind_open = false;
  bool overflowed_now = false; // consumed by the RX path to revoke the lease
  void note(uint32_t id, bool ext, bool rtr, uint8_t dlc, const uint8_t* data) {
    if (!active) return;
    ++all_rx;
    unsigned slot = used;
    for (unsigned i = 0; i < used; ++i)
      if (ids[i] == id && extended[i] == (ext ? 1U : 0U)) slot = i;
    if (slot == used) {
      if (used >= kIds) { ++overflow; overflowed_now = true; return; }
      ids[slot] = id; extended[slot] = ext ? 1U : 0U; ++used;
    }
    const unsigned pci = (!rtr && dlc) ? (data[0] >> 4) : 0xF;
    ++counts[slot][pci <= 3 ? pci : static_cast<unsigned>(Other)];
  }
};

class CanMonitor {
  friend struct signal_v2::CanOwnerTestAccess;
 public:
  // Start a fresh audit window (clears previous counts) / copy it.
  // The counter snapshot is taken in the same critical section as the audit
  // boundary, so per-ID counts and counter deltas cover the same frames.
  void beginObservationAudit(CanStats& baseline) {
    portENTER_CRITICAL(&stats_mux_);
    audit_ = ObservationAudit{};
    audit_.active = true;
    statsLocked(baseline);
    portEXIT_CRITICAL(&stats_mux_);
  }
  void endObservationAudit(ObservationAudit& copy, CanStats& final_stats) {
    portENTER_CRITICAL(&stats_mux_);
    audit_.active = false;
    audit_.rx_inflight_at_end = rx_processing_;
    copy = audit_;
    statsLocked(final_stats); // includes the charged-traffic windows (M-03)
    portEXIT_CRITICAL(&stats_mux_);
  }
  // Caller holds stats_mux_.
  void statsLocked(CanStats& copy) const {
    copy = stats_;
    observed_budget_.refresh(millis());
    copy.observed100 = observed_budget_.used100;
    copy.observed1000 = observed_budget_.used1000;
    copy.observed_peak100 = observed_budget_.peak100;
    copy.observed_peak1000 = observed_budget_.peak1000;
  }
  bool observationAuditOverflowed() const {
    portENTER_CRITICAL(&stats_mux_);
    const bool overflowed = audit_.overflow != 0;
    portEXIT_CRITICAL(&stats_mux_);
    return overflowed;
  }
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
  bool beginStageS1(uint32_t epoch);
  void revokeStageAuthorization(uint32_t epoch);

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
  void beginObservationBlind(uint64_t at_us);
  void endObservationBlind(uint64_t at_us);
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
  SemaphoreHandle_t tx_gate_ = nullptr; // Task mutex: blocking TWAI API allowed.

  mutable portMUX_TYPE query_mux_ = portMUX_INITIALIZER_UNLOCKED;
  mutable portMUX_TYPE stats_mux_ = portMUX_INITIALIZER_UNLOCKED;
  mutable portMUX_TYPE lease_mux_ = portMUX_INITIALIZER_UNLOCKED;
  CanStats stats_{};
  uint32_t rx_processing_ = 0; // Started RX awaiting audit_.note(); stats_mux_ protected.
  ObservationAudit audit_{}; // protected by stats_mux_
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
  // RX owner only: retain probe context during idle/pacing, across ARM.
  // A later admitted regular query explicitly takes over response ownership.
  bool probe_guard_=false,probe_receiving_=false;
  uint64_t probe_tx_us_=0,last_v2_start_us_=0;
  bool v2_start_seen_=false,last_v2_discovery_=false;
  uint64_t lease_deadline_ms_ = 0;
  uint32_t stage_epoch_ = 0, revoked_stage_epoch_ = 0; // tx_gate_ protected
  bool stage_authorized_ = false; // tx_gate_ protected
  char last_error_[96] = "not started";
};

}  // namespace m5can
