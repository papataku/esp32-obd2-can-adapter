#include "can_monitor.h"

#include <algorithm>
#include <cstring>
#include <esp_timer.h>

#include "app_config.h"

namespace m5can {
namespace {
struct QueryActiveReset {
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
  std::strncpy(last_error_, message ? message : "unknown", sizeof(last_error_) - 1);
  last_error_[sizeof(last_error_) - 1] = '\0';
}

bool CanMonitor::installDriver(twai_mode_t mode) {
  twai_general_config_t general =
      TWAI_GENERAL_CONFIG_DEFAULT(cfg::kCanTxPin, cfg::kCanRxPin, mode);
  general.tx_queue_len =
      mode == TWAI_MODE_NORMAL ? cfg::kTwaiNormalTxQueueLen : cfg::kTwaiListenTxQueueLen;
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

  // Boot invariant: receive-only.
  if (!installDriver(TWAI_MODE_LISTEN_ONLY)) {
    setError("listen-only driver init failed");
    return false;
  }

  running_ = true;
  if (xTaskCreatePinnedToCore(rxTaskThunk,
                              "m5can-owner",
                              6144,
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
  if (!installDriver(mode)) return false;
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
  if (!frame.rtr && frame.dlc) std::memcpy(frame.data, message.data, frame.dlc);

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
    const uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
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

  if (info.state == TWAI_STATE_BUS_OFF) {
    revokeLease();
  }
}

bool CanMonitor::acquireLease(uint32_t duration_ms) {
  if (fault_locked_) return false;
  if (duration_ms == 0) duration_ms = cfg::kTxLeaseDefaultMs;
  duration_ms = std::min(duration_ms, cfg::kTxLeaseMaxMs);
  const uint64_t now_ms = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
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
  const uint64_t now_ms = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
  uint64_t deadline = 0;
  portENTER_CRITICAL(&lease_mux_);
  deadline = lease_deadline_ms_;
  portEXIT_CRITICAL(&lease_mux_);
  if (deadline <= now_ms) return 0;
  const uint64_t remaining = deadline - now_ms;
  return remaining > 0xFFFFFFFFULL ? 0xFFFFFFFFU : static_cast<uint32_t>(remaining);
}

bool CanMonitor::leaseAllowsBudget(uint32_t budget_ms) const {
  return leaseRemainingMs() >= budget_ms;
}

bool CanMonitor::waitForRateLimit() {
  if (!last_tx_us_) return true;
  const uint64_t earliest = last_tx_us_ + static_cast<uint64_t>(cfg::kMinDiagnosticIntervalMs) * 1000ULL;
  while (static_cast<uint64_t>(esp_timer_get_time()) < earliest) {
    if (leaseRemainingMs() == 0) return false;
    twai_message_t message{};
    if (twai_receive(&message, pdMS_TO_TICKS(2)) == ESP_OK) {
      captureMessage(message, static_cast<uint64_t>(esp_timer_get_time()));
    }
  }
  return true;
}

void CanMonitor::enterFaultLocked(const char* reason) {
  fault_locked_ = true;
  revokeLease();
  setError(reason);
}

bool CanMonitor::appendSingleFrameResponse(const DiagnosticRequest& request,
                                           const CapturedFrame& frame,
                                           DiagnosticResult& result,
                                           bool& multi_frame_match) {
  multi_frame_match = false;
  if (!frame.extended || frame.rtr ||
      !DiagnosticPolicy::allowedResponseId(frame.identifier) || frame.dlc < 2) {
    return false;
  }

  const uint8_t pci_type = static_cast<uint8_t>(frame.data[0] >> 4);
  if (pci_type == 0x0) {
    const uint8_t payload_len = static_cast<uint8_t>(frame.data[0] & 0x0F);
    if (payload_len == 0 || payload_len > 7 ||
        static_cast<uint8_t>(payload_len + 1) > frame.dlc) {
      return false;
    }
    if (!DiagnosticPolicy::responseMatches(request, &frame.data[1], payload_len)) {
      return false;
    }
    if (result.response_count >= kDiagnosticMaxResponses) return true;
    DiagnosticMessage& response = result.responses[result.response_count++];
    response.can_id = frame.identifier;
    response.length = payload_len;
    std::memcpy(response.data, &frame.data[1], payload_len);
    return true;
  }

  if (pci_type == 0x1 && frame.dlc >= 4) {
    const uint16_t total_len =
        static_cast<uint16_t>(((frame.data[0] & 0x0F) << 8) | frame.data[1]);
    const size_t available = std::min<size_t>(6, total_len);
    if (DiagnosticPolicy::responseMatches(request, &frame.data[2], available)) {
      multi_frame_match = true;
      return true;
    }
  }
  return false;
}

DiagnosticResult CanMonitor::performQuery(const DiagnosticRequest& request) {
  DiagnosticResult result{};
  portENTER_CRITICAL(&stats_mux_);
  stats_.query_active = true;
  portEXIT_CRITICAL(&stats_mux_);
  QueryActiveReset query_active_reset{&stats_, &stats_mux_};
  const uint64_t transaction_start_us = static_cast<uint64_t>(esp_timer_get_time());

  portENTER_CRITICAL(&stats_mux_);
  ++stats_.query_count;
  portEXIT_CRITICAL(&stats_mux_);

  if (fault_locked_) {
    result.status = DiagnosticStatus::ModeError;
    return result;
  }
  if (!DiagnosticPolicy::allowedReadRequest(request)) {
    result.status = DiagnosticStatus::InvalidRequest;
    return result;
  }

  const uint32_t response_timeout_ms =
      std::max<uint32_t>(20, std::min<uint32_t>(request.timeout_ms, 1000));
  const uint32_t budget_ms = DiagnosticPolicy::transactionBudgetMs(response_timeout_ms);
  if (!leaseAllowsBudget(budget_ms)) {
    result.status = DiagnosticStatus::LeaseRequired;
    return result;
  }
  if (!waitForRateLimit() || !leaseAllowsBudget(budget_ms)) {
    result.status = DiagnosticStatus::LeaseRequired;
    return result;
  }

  if (!switchDriverMode(TWAI_MODE_NORMAL)) {
    enterFaultLocked("enter normal mode failed");
    result.status = DiagnosticStatus::ModeError;
    return result;
  }

  if (!leaseAllowsBudget(response_timeout_ms + 50)) {
    if (!switchDriverMode(TWAI_MODE_LISTEN_ONLY)) {
      enterFaultLocked("return listen-only failed");
      result.status = DiagnosticStatus::ModeError;
    } else {
      result.status = DiagnosticStatus::LeaseRequired;
    }
    return result;
  }

  twai_message_t tx{};
  tx.identifier = request.can_id;
  tx.extd = 1;
  tx.rtr = 0;
  tx.ss = 1;  // single-shot: never silently retransmit diagnostic traffic
  tx.data_length_code = 8;
  tx.data[0] = request.length;
  std::memcpy(&tx.data[1], request.data, request.length);

  portENTER_CRITICAL(&stats_mux_);
  ++stats_.tx_attempts;
  portEXIT_CRITICAL(&stats_mux_);

  const uint64_t tx_us = static_cast<uint64_t>(esp_timer_get_time());
  const esp_err_t tx_result = twai_transmit(&tx, pdMS_TO_TICKS(20));
  last_tx_us_ = tx_us;
  if (tx_result != ESP_OK) {
    result.status = DiagnosticStatus::DriverError;
  } else {
    portENTER_CRITICAL(&stats_mux_);
    ++stats_.tx_success;
    portEXIT_CRITICAL(&stats_mux_);

    const uint64_t deadline_us =
        tx_us + static_cast<uint64_t>(response_timeout_ms) * 1000ULL;
    uint64_t quiet_deadline_us = 0;
    bool multi_frame = false;

    while (true) {
      const uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
      const uint64_t effective_deadline =
          quiet_deadline_us ? std::min(deadline_us, quiet_deadline_us) : deadline_us;
      if (now_us >= effective_deadline) break;

      const uint32_t remaining_ms =
          static_cast<uint32_t>((effective_deadline - now_us + 999ULL) / 1000ULL);
      twai_message_t rx{};
      const esp_err_t rx_result =
          twai_receive(&rx, pdMS_TO_TICKS(std::min<uint32_t>(5, remaining_ms)));
      if (rx_result != ESP_OK) {
        updateDriverStats();
        const CanStats snapshot = stats();
        if (snapshot.state == TWAI_STATE_BUS_OFF) {
          result.status = DiagnosticStatus::BusOff;
          revokeLease();
          break;
        }
        continue;
      }

      const CapturedFrame frame =
          captureMessage(rx, static_cast<uint64_t>(esp_timer_get_time()));
      bool frame_is_multi = false;
      if (appendSingleFrameResponse(request, frame, result, frame_is_multi)) {
        if (!result.tx_to_first_response_us) {
          result.tx_to_first_response_us =
              static_cast<uint32_t>(frame.timestamp_us - tx_us);
        }
        if (frame_is_multi) {
          multi_frame = true;
          break;
        }
        quiet_deadline_us = frame.timestamp_us + cfg::kPostResponseQuietUs;
      }
    }

    if (result.status != DiagnosticStatus::BusOff) {
      if (multi_frame) result.status = DiagnosticStatus::MultiFrameRequired;
      else if (result.response_count) result.status = DiagnosticStatus::Ok;
      else {
        result.status = DiagnosticStatus::NoData;
        portENTER_CRITICAL(&stats_mux_);
        ++stats_.query_timeout;
        portEXIT_CRITICAL(&stats_mux_);
      }
    }
  }

  if (!switchDriverMode(TWAI_MODE_LISTEN_ONLY)) {
    enterFaultLocked("return listen-only failed");
    result.status = DiagnosticStatus::ModeError;
  }

  result.transaction_us =
      static_cast<uint32_t>(static_cast<uint64_t>(esp_timer_get_time()) -
                            transaction_start_us);
  return result;
}

bool CanMonitor::query(const DiagnosticRequest& request,
                       DiagnosticResult& result,
                       TickType_t wait_ticks) {
  if (!query_queue_ || !result_queue_ || fault_locked_) return false;

  DiagnosticResult stale{};
  while (xQueueReceive(result_queue_, &stale, 0) == pdTRUE) {}

  if (xQueueSend(query_queue_, &request, 0) != pdTRUE) return false;
  return xQueueReceive(result_queue_, &result, wait_ticks) == pdTRUE;
}

bool CanMonitor::pop(CapturedFrame& frame, TickType_t wait_ticks) {
  return frame_queue_ && xQueueReceive(frame_queue_, &frame, wait_ticks) == pdTRUE;
}

CanStats CanMonitor::stats() const {
  CanStats copy{};
  portENTER_CRITICAL(&stats_mux_);
  copy = stats_;
  portEXIT_CRITICAL(&stats_mux_);
  return copy;
}

}  // namespace m5can
