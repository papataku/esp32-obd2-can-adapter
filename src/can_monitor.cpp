#include "can_monitor.h"

#include <algorithm>
#include <cstring>
#include <esp_timer.h>

#include "app_config.h"
#include "diagnostic_transaction.h"

namespace m5can {
namespace {

struct QueryActiveReset {
  QueryActiveReset(CanStats* stats_in, portMUX_TYPE* mux_in)
      : stats(stats_in), mux(mux_in) {}
  CanStats* stats = nullptr;
  portMUX_TYPE* mux = nullptr;
  ~QueryActiveReset() {
    if (!stats || !mux) return;
    portENTER_CRITICAL(mux);
    stats->query_active = false;
    portEXIT_CRITICAL(mux);
  }
};

}  // namespace

void CanMonitor::setError(const char* message) {
  std::strncpy(last_error_, message ? message : "unknown",
               sizeof(last_error_) - 1);
  last_error_[sizeof(last_error_) - 1] = '\0';
}

bool CanMonitor::installDriver(twai_mode_t mode) {
  twai_general_config_t general =
      TWAI_GENERAL_CONFIG_DEFAULT(cfg::kCanTxPin, cfg::kCanRxPin, mode);
  general.tx_queue_len =
      mode == TWAI_MODE_NORMAL
          ? cfg::kTwaiNormalTxQueueLen
          : cfg::kTwaiListenTxQueueLen;
  general.rx_queue_len = cfg::kTwaiRxQueueLen;
  general.alerts_enabled = TWAI_ALERT_RX_QUEUE_FULL |
                           TWAI_ALERT_BUS_OFF |
                           TWAI_ALERT_BUS_RECOVERED |
                           TWAI_ALERT_ERR_PASS |
                           TWAI_ALERT_ABOVE_ERR_WARN |
                           TWAI_ALERT_BELOW_ERR_WARN;

  const twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
  const twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if (twai_driver_install(&general, &timing, &filter) != ESP_OK) return false;
  if (twai_start() != ESP_OK) {
    twai_driver_uninstall();
    return false;
  }
  return true;
}

bool CanMonitor::begin() {
  if (running_) return true;

  frame_queue_ = xQueueCreate(cfg::kFrameQueueLen, sizeof(CapturedFrame));
  query_queue_ = xQueueCreate(1, sizeof(DiagnosticRequest));
  result_queue_ = xQueueCreate(1, sizeof(DiagnosticResult));
  if (!frame_queue_ || !query_queue_ || !result_queue_) {
    setError("queue allocation failed");
    return false;
  }

  if (!installDriver(TWAI_MODE_LISTEN_ONLY)) {
    setError("listen-only driver init failed");
    return false;
  }

  running_ = true;
  if (xTaskCreatePinnedToCore(rxTaskThunk,
                              "m5can-owner",
                              7168,
                              this,
                              configMAX_PRIORITIES - 3,
                              &rx_task_,
                              0) != pdPASS) {
    running_ = false;
    twai_stop();
    twai_driver_uninstall();
    setError("CAN owner task creation failed");
    return false;
  }

  setError("OK");
  return true;
}

bool CanMonitor::switchDriverMode(twai_mode_t mode) {
  const esp_err_t stopped = twai_stop();
  if (stopped != ESP_OK && stopped != ESP_ERR_INVALID_STATE) return false;
  if (twai_driver_uninstall() != ESP_OK) return false;

  if (!installDriver(mode)) {
    if (mode != TWAI_MODE_LISTEN_ONLY) {
      installDriver(TWAI_MODE_LISTEN_ONLY);
    }
    return false;
  }

  updateDriverStats();
  return true;
}

void CanMonitor::rxTaskThunk(void* arg) {
  static_cast<CanMonitor*>(arg)->rxTask();
}

CapturedFrame CanMonitor::captureMessage(const twai_message_t& message,
                                         uint64_t timestamp_us) {
  CapturedFrame frame{};
  frame.timestamp_us = timestamp_us;
  frame.identifier = message.identifier;
  frame.dlc = message.data_length_code > 8 ? 8 : message.data_length_code;
  frame.extended = message.extd;
  frame.rtr = message.rtr;
  frame.self = message.self;
  if (!frame.rtr && frame.dlc) {
    std::memcpy(frame.data, message.data, frame.dlc);
  }

  portENTER_CRITICAL(&stats_mux_);
  ++stats_.rx_frames;
  if (frame.extended) ++stats_.ext_frames;
  else ++stats_.std_frames;
  if (frame.rtr) ++stats_.rtr_frames;
  portEXIT_CRITICAL(&stats_mux_);

  if (xQueueSend(frame_queue_, &frame, 0) != pdTRUE) {
    portENTER_CRITICAL(&stats_mux_);
    ++stats_.app_queue_drops;
    portEXIT_CRITICAL(&stats_mux_);
  }
  return frame;
}

void CanMonitor::rxTask() {
  while (running_) {
    DiagnosticRequest request{};
    if (xQueueReceive(query_queue_, &request, 0) == pdTRUE) {
      const DiagnosticResult result = performQuery(request);
      xQueueSend(result_queue_, &result, 0);
      continue;
    }

    twai_message_t message{};
    const esp_err_t receive_result =
        twai_receive(&message, pdMS_TO_TICKS(cfg::kTwaiReceiveWaitMs));
    const uint64_t now_us =
        static_cast<uint64_t>(esp_timer_get_time());
    if (receive_result == ESP_OK) captureMessage(message, now_us);
    maybeUpdateDriverStats(now_us);
    if (receive_result != ESP_OK) taskYIELD();
  }

  rx_task_ = nullptr;
  vTaskDelete(nullptr);
}

void CanMonitor::maybeUpdateDriverStats(uint64_t now_us) {
  if (now_us - last_status_poll_us_ < 100000U) return;
  last_status_poll_us_ = now_us;
  updateDriverStats();
}

void CanMonitor::updateDriverStats() {
  twai_status_info_t info{};
  if (twai_get_status_info(&info) != ESP_OK) return;

  portENTER_CRITICAL(&stats_mux_);
  stats_.driver_rx_missed = info.rx_missed_count;
  stats_.driver_rx_overrun = info.rx_overrun_count;
  stats_.driver_bus_error = info.bus_error_count;
  stats_.driver_arb_lost = info.arb_lost_count;
  stats_.rx_queue_depth = info.msgs_to_rx;
  stats_.state = info.state;
  portEXIT_CRITICAL(&stats_mux_);

  if (info.state == TWAI_STATE_BUS_OFF) revokeLease();
}

bool CanMonitor::acquireLease(uint32_t duration_ms) {
  if (fault_locked_) return false;
  if (duration_ms == 0) duration_ms = cfg::kTxLeaseDefaultMs;
  duration_ms = std::min(duration_ms, cfg::kTxLeaseMaxMs);
  const uint64_t now_ms =
      static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
  portENTER_CRITICAL(&lease_mux_);
  lease_deadline_ms_ = now_ms + duration_ms;
  portEXIT_CRITICAL(&lease_mux_);
  return true;
}

void CanMonitor::revokeLease() {
  portENTER_CRITICAL(&lease_mux_);
  lease_deadline_ms_ = 0;
  portEXIT_CRITICAL(&lease_mux_);
}

uint32_t CanMonitor::leaseRemainingMs() const {
  const uint64_t now_ms =
      static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
  uint64_t deadline = 0;
  portENTER_CRITICAL(&lease_mux_);
  deadline = lease_deadline_ms_;
  portEXIT_CRITICAL(&lease_mux_);
  if (deadline <= now_ms) return 0;
  const uint64_t remaining = deadline - now_ms;
  return remaining > 0xFFFFFFFFULL
             ? 0xFFFFFFFFU
             : static_cast<uint32_t>(remaining);
}

bool CanMonitor::leaseAllowsBudget(uint32_t budget_ms) const {
  return leaseRemainingMs() >= budget_ms;
}

bool CanMonitor::waitForRateLimit(uint32_t request_can_id) {
  // Different approved CAN IDs have independent 50-ms diagnostic clocks.
  // The CAN owner still permits only one in-flight ECU transaction.
  while (diagnostic_pacer_.remainingUs(
             request_can_id,
             static_cast<uint64_t>(esp_timer_get_time())) > 0) {
    if (leaseRemainingMs() == 0) return false;
    twai_message_t message{};
    if (twai_receive(&message, pdMS_TO_TICKS(2)) == ESP_OK) {
      captureMessage(message,
                     static_cast<uint64_t>(esp_timer_get_time()));
    }
  }
  return true;
}

void CanMonitor::enterFaultLocked(const char* reason) {
  fault_locked_ = true;
  revokeLease();
  setError(reason);
}

esp_err_t CanMonitor::transmitOwnedFrame(const twai_message_t& message,
                                         uint32_t wait_ms) {
  portENTER_CRITICAL(&stats_mux_);
  ++stats_.tx_attempts;
  portEXIT_CRITICAL(&stats_mux_);

  const esp_err_t result =
      twai_transmit(&message, pdMS_TO_TICKS(wait_ms));
  if (result == ESP_OK) {
    portENTER_CRITICAL(&stats_mux_);
    ++stats_.tx_success;
    portEXIT_CRITICAL(&stats_mux_);
  }
  return result;
}

twai_message_t CanMonitor::makeDiagnosticRequestFrame(
    const DiagnosticRequest& request) {
  twai_message_t tx{};
  tx.identifier = request.can_id;
  tx.extd = 1;
  tx.rtr = 0;
  tx.ss = 1;
  tx.data_length_code = 8;
  tx.data[0] = request.length;
  std::memcpy(&tx.data[1], request.data, request.length);
  return tx;
}

twai_message_t CanMonitor::makeFlowControlFrame(uint32_t can_id) {
  twai_message_t tx{};
  tx.identifier = can_id;
  tx.extd = 1;
  tx.rtr = 0;
  tx.ss = 1;
  tx.data_length_code = 8;
  tx.data[0] = 0x30;
  tx.data[1] = 0x00;
  tx.data[2] = 0x00;
  return tx;
}

DiagnosticResult CanMonitor::performQuery(
    const DiagnosticRequest& request) {
  DiagnosticResult result{};

  portENTER_CRITICAL(&stats_mux_);
  stats_.query_active = true;
  ++stats_.query_count;
  portEXIT_CRITICAL(&stats_mux_);
  QueryActiveReset query_active_reset{&stats_, &stats_mux_};

  const uint64_t transaction_start_us =
      static_cast<uint64_t>(esp_timer_get_time());

  if (fault_locked_) {
    result.status = DiagnosticStatus::ModeError;
    return result;
  }
  if (!DiagnosticPolicy::allowedReadRequest(request)) {
    result.status = DiagnosticStatus::InvalidRequest;
    return result;
  }

  const uint32_t response_timeout_ms =
      std::max<uint32_t>(
          20, std::min<uint32_t>(request.timeout_ms, 1000));
  const bool allow_response_pending = request.data[0] == 0x22;
  const uint32_t budget_ms =
      DiagnosticPolicy::transactionBudgetMs(
          response_timeout_ms, allow_response_pending);

  if (!leaseAllowsBudget(budget_ms)) {
    result.status = DiagnosticStatus::LeaseRequired;
    return result;
  }
  if (!waitForRateLimit(request.can_id) || !leaseAllowsBudget(budget_ms)) {
    result.status = DiagnosticStatus::LeaseRequired;
    return result;
  }

  if (!switchDriverMode(TWAI_MODE_NORMAL)) {
    enterFaultLocked("enter normal mode failed");
    result.status = DiagnosticStatus::ModeError;
    return result;
  }

  DiagnosticStatus terminal = DiagnosticStatus::NoData;
  bool terminal_set = false;

  const uint32_t max_window_ms =
      allow_response_pending
          ? std::max(response_timeout_ms,
                     cfg::kMaxResponsePendingWindowMs)
          : response_timeout_ms;

  if (!leaseAllowsBudget(max_window_ms + 50)) {
    terminal = DiagnosticStatus::LeaseRequired;
    terminal_set = true;
  }

  uint64_t tx_us = 0;
  if (!terminal_set) {
    const twai_message_t tx =
        makeDiagnosticRequestFrame(request);
    tx_us = static_cast<uint64_t>(esp_timer_get_time());
    if (transmitOwnedFrame(tx, 20) != ESP_OK) {
      terminal = DiagnosticStatus::DriverError;
      terminal_set = true;
    } else {
      diagnostic_pacer_.noteSuccessfulTx(request.can_id, tx_us);
    }
  }

  DiagnosticTransaction transaction;
  if (!terminal_set) {
    transaction.begin(
        request, tx_us,
        static_cast<uint32_t>(cfg::kIsoTpCfTimeoutUs / 1000ULL));
  }

  uint64_t deadline_us =
      tx_us + static_cast<uint64_t>(response_timeout_ms) * 1000ULL;
  const uint64_t hard_deadline_us =
      tx_us + static_cast<uint64_t>(max_window_ms) * 1000ULL;
  uint64_t quiet_deadline_us = 0;

  while (!terminal_set) {
    const uint64_t now_us =
        static_cast<uint64_t>(esp_timer_get_time());

    if (transaction.expireIsoTp(now_us) > 0) {
      terminal = DiagnosticStatus::IsoTpError;
      terminal_set = true;
      break;
    }

    uint64_t effective_deadline_us = deadline_us;
    if (quiet_deadline_us && !transaction.hasActiveIsoTp()) {
      effective_deadline_us =
          std::min(effective_deadline_us, quiet_deadline_us);
    }
    if (now_us >= effective_deadline_us) break;

    const uint32_t remaining_ms =
        static_cast<uint32_t>(
            (effective_deadline_us - now_us + 999ULL) / 1000ULL);
    twai_message_t rx{};
    const esp_err_t receive_result =
        twai_receive(&rx,
                     pdMS_TO_TICKS(
                         std::min<uint32_t>(5, remaining_ms)));

    if (receive_result != ESP_OK) {
      updateDriverStats();
      const CanStats snapshot = stats();
      if (snapshot.state == TWAI_STATE_BUS_OFF) {
        terminal = DiagnosticStatus::BusOff;
        terminal_set = true;
        revokeLease();
      }
      continue;
    }

    const CapturedFrame captured =
        captureMessage(
            rx, static_cast<uint64_t>(esp_timer_get_time()));

    TransactionFrame frame{};
    frame.can_id = captured.identifier;
    frame.extended = captured.extended;
    frame.rtr = captured.rtr;
    frame.dlc = captured.dlc;
    frame.timestamp_us = captured.timestamp_us;
    std::memcpy(frame.data, captured.data, captured.dlc);

    const TransactionStep step = transaction.onFrame(frame);

    if (step.event == TransactionEvent::NeedFlowControl) {
      if (!leaseAllowsBudget(50)) {
        terminal = DiagnosticStatus::LeaseRequired;
        terminal_set = true;
        break;
      }

      const twai_message_t fc =
          makeFlowControlFrame(step.flow_control_can_id);
      if (transmitOwnedFrame(fc, 20) != ESP_OK) {
        terminal = DiagnosticStatus::DriverError;
        terminal_set = true;
        break;
      }
      quiet_deadline_us = 0;
    } else if (step.event == TransactionEvent::Completed) {
      if (!transaction.hasActiveIsoTp()) {
        quiet_deadline_us =
            captured.timestamp_us + cfg::kPostResponseQuietUs;
      }
    } else if (step.event == TransactionEvent::ResponsePending) {
      if (!allow_response_pending ||
          !leaseAllowsBudget(
              cfg::kResponsePendingExtensionMs + 50)) {
        terminal = DiagnosticStatus::LeaseRequired;
        terminal_set = true;
        break;
      }

      deadline_us =
          std::min<uint64_t>(
              hard_deadline_us,
              captured.timestamp_us +
                  static_cast<uint64_t>(
                      cfg::kResponsePendingExtensionMs) *
                      1000ULL);
      quiet_deadline_us = 0;
    } else if (step.event == TransactionEvent::ProtocolError) {
      terminal = DiagnosticStatus::IsoTpError;
      terminal_set = true;
      break;
    }
  }

  if (!terminal_set) {
    result = transaction.result();
    if (result.response_count) {
      terminal = DiagnosticStatus::Ok;
    } else if (transaction.hasActiveIsoTp()) {
      terminal = DiagnosticStatus::IsoTpError;
    } else {
      terminal = DiagnosticStatus::NoData;
      portENTER_CRITICAL(&stats_mux_);
      ++stats_.query_timeout;
      portEXIT_CRITICAL(&stats_mux_);
    }
  } else if (tx_us) {
    result = transaction.result();
  }

  result.status = terminal;

  if (!switchDriverMode(TWAI_MODE_LISTEN_ONLY)) {
    enterFaultLocked("return listen-only failed");
    result.status = DiagnosticStatus::ModeError;
  }

  result.transaction_us =
      static_cast<uint32_t>(
          static_cast<uint64_t>(esp_timer_get_time()) -
          transaction_start_us);
  return result;
}

bool CanMonitor::submitQuery(const DiagnosticRequest& request) {
  if (!query_queue_ || !result_queue_ ||
      fault_locked_ || query_in_flight_) {
    return false;
  }

  DiagnosticResult stale{};
  while (xQueueReceive(result_queue_, &stale, 0) == pdTRUE) {}

  if (xQueueSend(query_queue_, &request, 0) != pdTRUE) return false;
  query_in_flight_ = true;
  return true;
}

bool CanMonitor::pollQueryResult(DiagnosticResult& result) {
  if (!query_in_flight_ || !result_queue_) return false;
  if (xQueueReceive(result_queue_, &result, 0) != pdTRUE) return false;
  query_in_flight_ = false;
  return true;
}

bool CanMonitor::query(const DiagnosticRequest& request,
                       DiagnosticResult& result,
                       TickType_t wait_ticks) {
  if (!submitQuery(request)) return false;
  if (xQueueReceive(result_queue_, &result, wait_ticks) != pdTRUE) {
    return false;
  }
  query_in_flight_ = false;
  return true;
}

bool CanMonitor::pop(CapturedFrame& frame, TickType_t wait_ticks) {
  return frame_queue_ &&
         xQueueReceive(frame_queue_, &frame, wait_ticks) == pdTRUE;
}

CanStats CanMonitor::stats() const {
  CanStats copy{};
  portENTER_CRITICAL(&stats_mux_);
  copy = stats_;
  portEXIT_CRITICAL(&stats_mux_);
  return copy;
}

}  // namespace m5can
