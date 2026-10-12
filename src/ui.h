#pragma once

#include <Arduino.h>
#include "can_monitor.h"
#include "ble_elm_transport.h"
#include "demo_touch_control.h"
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
#include "ble_elm_transport.h"
#endif

namespace m5can {
class Ui {
 public:
  void begin();
  // Updates the M5Dial touch hardware every loop. Call before update().
  bool pollDemoToggle();
  void update(const CanMonitor& can, const BleStats& ble_stats, bool ble_ready,
              bool ble_connected, bool ble_elm, bool demo_auto,
              bool demo_active
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
              , const BlePeersSnapshot* peers = nullptr
#endif
              );
  void showFatal(const char* title, const char* detail);

 private:
  DemoTouchControl demo_touch_{};
  uint32_t last_refresh_ms_ = 0;
  uint64_t last_rx_frames_ = 0;
  uint64_t last_query_count_ = 0;
  uint64_t last_tx_success_ = 0;
  uint32_t last_rate_ms_ = 0;
  uint32_t frames_per_second_ = 0;
  uint32_t queries_per_second_x10_ = 0;
  uint32_t tx_per_second_x10_ = 0;
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  BlePeersSnapshot last_peers_{};
  uint32_t last_peer_rate_ms_ = 0;
  uint32_t peer_rx_bytes_per_sec_[2]{};
  uint32_t peer_tx_bytes_per_sec_[2]{};
  bool show_ble_peer_page_ = true;
  char cached_lower_label_[32]{};
#endif

  char cached_ble_[32]{};
  char cached_status_[24]{};
  char cached_demo_mode_[24]{};
  char cached_fps_[20]{};
  char cached_rx_[32]{};
  char cached_diag_[48]{};
  char cached_tx_[40]{};
  char cached_errors_[48]{};

  uint16_t cached_ble_color_ = 0xFFFF;
  uint16_t cached_status_color_ = 0xFFFF;
  uint16_t cached_demo_mode_color_ = 0xFFFF;
  uint16_t cached_tx_color_ = 0xFFFF;
  uint16_t cached_error_color_ = 0xFFFF;
};
}  // namespace m5can
