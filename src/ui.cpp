#include "ui.h"

#include <M5Dial.h>

#include "app_config.h"

namespace m5can {
namespace {

uint16_t statusColor(bool fault, bool query_active, uint32_t fps) {
  if (fault) return TFT_RED;
  if (query_active) return TFT_WHITE;
  if (fps > 0) return TFT_GREEN;
  return TFT_LIGHTGREY;
}

const char* statusText(bool fault, bool query_active, uint32_t fps) {
  if (fault) return "CAN ERROR";
  if (query_active) return "QUERY";
  if (fps > 0) return "CAN LIVE";
  return "CAN IDLE";
}

const char* bleText(bool ready, bool connected, bool elm) {
  if (!ready) return "BLE ERROR";
  if (elm) return "BLE ELM";
  if (connected) return "BLE LINK";
  return "BLE WAIT";
}

uint16_t bleColor(bool ready, bool connected, bool elm) {
  if (!ready) return TFT_RED;
  if (elm) return TFT_GREEN;
  if (connected) return TFT_WHITE;
  return TFT_LIGHTGREY;
}

}  // namespace

void Ui::begin() {
  auto config = M5.config();
  config.serial_baudrate = 0;
  config.clear_display = true;
  config.output_power = true;
  M5Dial.begin(config, true, false);
  M5Dial.Display.setRotation(0);
  M5Dial.Display.fillScreen(TFT_BLACK);
  M5Dial.Display.setTextDatum(middle_center);
}

void Ui::showFatal(const char* title, const char* detail) {
  M5Dial.Display.fillScreen(TFT_BLACK);
  M5Dial.Display.setTextDatum(middle_center);
  M5Dial.Display.setTextColor(TFT_RED, TFT_BLACK);
  M5Dial.Display.drawString(title ? title : "FATAL", 120, 95, &fonts::Font4);
  M5Dial.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Dial.Display.drawString(detail ? detail : "unknown", 120, 135, &fonts::Font2);
}

void Ui::update(const CanMonitor& can, bool ble_ready,
                bool ble_connected, bool ble_elm) {
  M5Dial.update();
  const uint32_t now = millis();
  if (now - last_refresh_ms_ < cfg::kDisplayRefreshMs) return;
  last_refresh_ms_ = now;

  const CanStats stats = can.stats();
  if (last_rate_ms_ == 0) {
    last_rate_ms_ = now;
    last_rx_frames_ = stats.rx_frames;
    last_query_count_ = stats.query_count;
    last_tx_success_ = stats.tx_success;
  }

  const uint32_t elapsed = now - last_rate_ms_;
  if (elapsed >= 1000) {
    frames_per_second_ =
        static_cast<uint32_t>(
            (stats.rx_frames - last_rx_frames_) * 1000ULL / elapsed);
    queries_per_second_x10_ =
        static_cast<uint32_t>(
            (stats.query_count - last_query_count_) * 10000ULL / elapsed);
    tx_per_second_x10_ =
        static_cast<uint32_t>(
            (stats.tx_success - last_tx_success_) * 10000ULL / elapsed);

    last_rx_frames_ = stats.rx_frames;
    last_query_count_ = stats.query_count;
    last_tx_success_ = stats.tx_success;
    last_rate_ms_ = now;
  }

  const bool fault =
      can.faultLocked() || stats.state == TWAI_STATE_BUS_OFF;
  const uint32_t lease_ms = can.leaseRemainingMs();

  char fps[20]{};
  char rx_total[28]{};
  char diag_rate[36]{};
  char tx_state[32]{};
  char errors[40]{};

  snprintf(fps, sizeof(fps), "%lu",
           static_cast<unsigned long>(frames_per_second_));
  snprintf(rx_total, sizeof(rx_total), "RX %llu",
           static_cast<unsigned long long>(stats.rx_frames));
  snprintf(diag_rate, sizeof(diag_rate),
           "DIAG %lu.%lu/s  TX %lu.%lu/s",
           static_cast<unsigned long>(queries_per_second_x10_ / 10),
           static_cast<unsigned long>(queries_per_second_x10_ % 10),
           static_cast<unsigned long>(tx_per_second_x10_ / 10),
           static_cast<unsigned long>(tx_per_second_x10_ % 10));
  if (lease_ms) {
    snprintf(tx_state, sizeof(tx_state), "TX LEASE %lu.%lus",
             static_cast<unsigned long>(lease_ms / 1000),
             static_cast<unsigned long>((lease_ms % 1000) / 100));
  } else {
    snprintf(tx_state, sizeof(tx_state), "TX LOCKED");
  }
  snprintf(errors, sizeof(errors), "D%llu M%lu O%lu B%lu",
           static_cast<unsigned long long>(stats.app_queue_drops),
           static_cast<unsigned long>(stats.driver_rx_missed),
           static_cast<unsigned long>(stats.driver_rx_overrun),
           static_cast<unsigned long>(stats.driver_bus_error));

  M5Dial.Display.fillScreen(TFT_BLACK);
  M5Dial.Display.setTextDatum(middle_center);

  M5Dial.Display.setTextColor(
      bleColor(ble_ready, ble_connected, ble_elm), TFT_BLACK);
  M5Dial.Display.drawString(
      bleText(ble_ready, ble_connected, ble_elm),
      120, 13, &fonts::Font2);

  M5Dial.Display.setTextColor(
      statusColor(fault, stats.query_active, frames_per_second_), TFT_BLACK);
  M5Dial.Display.drawString(
      statusText(fault, stats.query_active, frames_per_second_),
      120, 37, &fonts::Font4);

  M5Dial.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Dial.Display.drawString(fps, 120, 89, &fonts::Font4);
  M5Dial.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  M5Dial.Display.drawString("CAN frame/s", 120, 121, &fonts::Font2);

  M5Dial.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Dial.Display.drawString(rx_total, 120, 145, &fonts::Font2);
  M5Dial.Display.drawString(diag_rate, 120, 166, &fonts::Font2);

  M5Dial.Display.setTextColor(
      lease_ms ? TFT_WHITE : TFT_LIGHTGREY, TFT_BLACK);
  M5Dial.Display.drawString(tx_state, 120, 190, &fonts::Font2);

  const bool has_errors =
      stats.app_queue_drops || stats.driver_rx_missed ||
      stats.driver_rx_overrun || stats.driver_bus_error;
  M5Dial.Display.setTextColor(
      has_errors ? TFT_RED : TFT_LIGHTGREY, TFT_BLACK);
  M5Dial.Display.drawString(errors, 120, 213, &fonts::Font2);
}

}  // namespace m5can
