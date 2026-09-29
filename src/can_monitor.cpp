#include "can_monitor.h"

#include <cstring>
#include <esp_timer.h>
#include "app_config.h"

namespace m5can {

void CanMonitor::setError(const char* message) {
  std::strncpy(last_error_, message ? message : "unknown", sizeof(last_error_) - 1);
  last_error_[sizeof(last_error_) - 1] = '\0';
}

bool CanMonitor::begin() {
  if (running_) return true;
  frame_queue_ = xQueueCreate(cfg::kFrameQueueLen, sizeof(CapturedFrame));
  if (!frame_queue_) { setError("frame queue allocation failed"); return false; }

  twai_general_config_t general =
      TWAI_GENERAL_CONFIG_DEFAULT(cfg::kCanTxPin, cfg::kCanRxPin, TWAI_MODE_LISTEN_ONLY);
  general.tx_queue_len = 0;
  general.rx_queue_len = cfg::kTwaiRxQueueLen;
  general.alerts_enabled = TWAI_ALERT_RX_QUEUE_FULL | TWAI_ALERT_BUS_OFF |
                           TWAI_ALERT_BUS_RECOVERED | TWAI_ALERT_ERR_PASS |
                           TWAI_ALERT_ABOVE_ERR_WARN | TWAI_ALERT_BELOW_ERR_WARN;

  const auto timing = TWAI_TIMING_CONFIG_500KBITS();
  const auto filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if (twai_driver_install(&general, &timing, &filter) != ESP_OK) {
    vQueueDelete(frame_queue_); frame_queue_ = nullptr;
    setError("twai_driver_install failed"); return false;
  }
  if (twai_start() != ESP_OK) {
    twai_driver_uninstall(); vQueueDelete(frame_queue_); frame_queue_ = nullptr;
    setError("twai_start failed"); return false;
  }

  running_ = true;
  if (xTaskCreatePinnedToCore(rxTaskThunk, "m5can-rx", 4096, this,
                              configMAX_PRIORITIES - 3, &rx_task_, 0) != pdPASS) {
    running_ = false;
    twai_stop(); twai_driver_uninstall();
    vQueueDelete(frame_queue_); frame_queue_ = nullptr;
    setError("rx task creation failed"); return false;
  }
  setError("OK");
  return true;
}

void CanMonitor::rxTaskThunk(void* arg) {
  static_cast<CanMonitor*>(arg)->rxTask();
}

void CanMonitor::rxTask() {
  while (running_) {
    twai_message_t message{};
    if (twai_receive(&message, pdMS_TO_TICKS(cfg::kTwaiReceiveWaitMs)) != ESP_OK) {
      const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
      if (now - last_status_poll_us_ >= 100000U) {
        last_status_poll_us_ = now;
        updateDriverStats();
      }
      taskYIELD();
      continue;
    }

    CapturedFrame frame{};
    frame.timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
    frame.identifier = message.identifier;
    frame.dlc = message.data_length_code > 8 ? 8 : message.data_length_code;
    frame.extended = message.extd;
    frame.rtr = message.rtr;
    frame.self = message.self;
    if (!frame.rtr && frame.dlc) std::memcpy(frame.data, message.data, frame.dlc);

    portENTER_CRITICAL(&stats_mux_);
    ++stats_.rx_frames;
    if (frame.extended) ++stats_.ext_frames; else ++stats_.std_frames;
    if (frame.rtr) ++stats_.rtr_frames;
    portEXIT_CRITICAL(&stats_mux_);

    if (xQueueSend(frame_queue_, &frame, 0) != pdTRUE) {
      portENTER_CRITICAL(&stats_mux_);
      ++stats_.app_queue_drops;
      portEXIT_CRITICAL(&stats_mux_);
    }
  }
  rx_task_ = nullptr;
  vTaskDelete(nullptr);
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
