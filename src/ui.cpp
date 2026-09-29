#include "ui.h"
#include <M5Dial.h>
#include "app_config.h"

namespace m5can {

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
  M5Dial.Display.setTextColor(TFT_RED, TFT_BLACK);
  M5Dial.Display.drawString(title ? title : "FATAL", 120, 95, &fonts::Font4);
  M5Dial.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Dial.Display.drawString(detail ? detail : "unknown", 120, 135, &fonts::Font2);
}

void Ui::update(const CanMonitor& can) {
  M5Dial.update();
  const uint32_t now = millis();
  if (now - last_refresh_ms_ < cfg::kDisplayRefreshMs) return;
  last_refresh_ms_ = now;
  const CanStats stats = can.stats();
  if (last_rate_ms_ == 0) last_rate_ms_ = now;
  const uint32_t elapsed = now - last_rate_ms_;
  if (elapsed >= 1000) {
    frames_per_second_ =
        static_cast<uint32_t>((stats.rx_frames - last_rx_frames_) * 1000ULL / elapsed);
    last_rx_frames_ = stats.rx_frames;
    last_rate_ms_ = now;
  }
  char rx[32]{}, rate[32]{}, drops[32]{};
  snprintf(rx, sizeof(rx), "RX %llu", static_cast<unsigned long long>(stats.rx_frames));
  snprintf(rate, sizeof(rate), "%lu frame/s", static_cast<unsigned long>(frames_per_second_));
  snprintf(drops, sizeof(drops), "DROP %llu",
           static_cast<unsigned long long>(stats.app_queue_drops));

  M5Dial.Display.fillScreen(TFT_BLACK);
  M5Dial.Display.setTextDatum(middle_center);
  M5Dial.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Dial.Display.drawString("M5CAN", 120, 42, &fonts::Font4);
  M5Dial.Display.setTextColor(TFT_GREEN, TFT_BLACK);
  M5Dial.Display.drawString("LISTEN ONLY", 120, 78, &fonts::Font2);
  M5Dial.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Dial.Display.drawString("CAN 500k", 120, 108, &fonts::Font2);
  M5Dial.Display.drawString(rx, 120, 136, &fonts::Font2);
  M5Dial.Display.drawString(rate, 120, 160, &fonts::Font2);
  M5Dial.Display.drawString(drops, 120, 184, &fonts::Font2);
  M5Dial.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  M5Dial.Display.drawString("TX DISABLED", 120, 212, &fonts::Font2);
}

}  // namespace m5can
