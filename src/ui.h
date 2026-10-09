#pragma once
#include <Arduino.h>
#include "can_monitor.h"

namespace m5can {
class Ui {
 public:
  void begin();
  void update(const CanMonitor& can, bool ble_ready,
              bool ble_connected, bool ble_elm);
  void showFatal(const char* title, const char* detail);

 private:
  uint32_t last_refresh_ms_ = 0;
  uint64_t last_rx_frames_ = 0;
  uint64_t last_query_count_ = 0;
  uint64_t last_tx_success_ = 0;
  uint32_t last_rate_ms_ = 0;
  uint32_t frames_per_second_ = 0;
  uint32_t queries_per_second_x10_ = 0;
  uint32_t tx_per_second_x10_ = 0;
};
}  // namespace m5can
