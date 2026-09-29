#include <Arduino.h>
#include "app_config.h"
#include "can_monitor.h"
#include "ui.h"

namespace {
m5can::CanMonitor g_can;
m5can::Ui g_ui;
uint32_t g_last_stats_ms = 0;

void emitFrame(const m5can::CapturedFrame& frame) {
  char data_hex[17]{};
  static constexpr char kHex[] = "0123456789ABCDEF";
  if (!frame.rtr) {
    for (uint8_t i = 0; i < frame.dlc; ++i) {
      data_hex[i * 2] = kHex[(frame.data[i] >> 4) & 0x0F];
      data_hex[i * 2 + 1] = kHex[frame.data[i] & 0x0F];
    }
  }
  Serial.printf(frame.extended
                    ? "@FRAME,%llu,E,%08lX,%u,%s,%s\n"
                    : "@FRAME,%llu,S,%03lX,%u,%s,%s\n",
                static_cast<unsigned long long>(frame.timestamp_us),
                static_cast<unsigned long>(frame.identifier),
                static_cast<unsigned>(frame.dlc), data_hex,
                frame.rtr ? "RTR" : "DATA");
}

void emitStats() {
  const auto s = g_can.stats();
  Serial.printf("#STATS,rx=%llu,std=%llu,ext=%llu,rtr=%llu,drop=%llu,missed=%lu,overrun=%lu,buserr=%lu,arb_lost=%lu,driver_q=%lu\n",
      static_cast<unsigned long long>(s.rx_frames),
      static_cast<unsigned long long>(s.std_frames),
      static_cast<unsigned long long>(s.ext_frames),
      static_cast<unsigned long long>(s.rtr_frames),
      static_cast<unsigned long long>(s.app_queue_drops),
      static_cast<unsigned long>(s.driver_rx_missed),
      static_cast<unsigned long>(s.driver_rx_overrun),
      static_cast<unsigned long>(s.driver_bus_error),
      static_cast<unsigned long>(s.driver_arb_lost),
      static_cast<unsigned long>(s.rx_queue_depth));
}
}

void setup() {
  Serial.begin(m5can::cfg::kSerialBaud);
  delay(80);
  g_ui.begin();
  Serial.printf("#HELLO,%s,%s,mode=LISTEN_ONLY,bitrate=%lu,tx=DISABLED\n",
                m5can::cfg::kFirmwareName, m5can::cfg::kFirmwareVersion,
                static_cast<unsigned long>(m5can::cfg::kCanBitrate));
  if (!g_can.begin()) {
    Serial.printf("#FATAL,CAN_INIT,%s\n", g_can.lastError());
    g_ui.showFatal("CAN INIT FAIL", g_can.lastError());
    return;
  }
  Serial.println("#READY,Phase 1 receive-only CAN monitor");
}

void loop() {
  m5can::CapturedFrame frame{};
  for (size_t i = 0; i < 64 && g_can.pop(frame, 0); ++i) emitFrame(frame);
  const uint32_t now = millis();
  if (now - g_last_stats_ms >= m5can::cfg::kStatsEmitMs) {
    g_last_stats_ms = now;
    emitStats();
  }
  g_ui.update(g_can);
  delay(1);
}
