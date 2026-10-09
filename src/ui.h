#pragma once

#include <Arduino.h>
#include <M5Dial.h>

#include "can_monitor.h"

namespace m5can {
class Ui {
 public:
  void begin();
  void update(const CanMonitor& can, bool ble_ready,
              bool ble_connected, bool ble_elm);
  void showFatal(const char* title, const char* detail);

 private:
  bool updateLine(char* cache, size_t cache_size,
                  const char* text, int32_t y, int32_t h,
                  uint16_t color, const lgfx::IFont* font);

  uint32_t last_refresh_ms_ = 0;
  uint64_t last_rx_frames_ = 0;
  uint64_t last_query_count_ = 0;
  uint64_t last_tx_success_ = 0;
  uint32_t last_rate_ms_ = 0;
  uint32_t frames_per_second_ = 0;
  uint32_t queries_per_second_x10_ = 0;
  uint32_t tx_per_second_x10_ = 0;

  char cached_ble_[24]{};
  char cached_status_[24]{};
  char cached_fps_[20]{};
  char cached_rx_[32]{};
  char cached_diag_[48]{};
  char cached_tx_[40]{};
  char cached_errors_[48]{};

  uint16_t cached_ble_color_ = 0xFFFF;
  uint16_t cached_status_color_ = 0xFFFF;
  uint16_t cached_tx_color_ = 0xFFFF;
  uint16_t cached_error_color_ = 0xFFFF;
};
}  // namespace m5can
