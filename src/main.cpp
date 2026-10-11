#include <Arduino.h>

#include "ble_elm_transport.h"
#include "can_monitor.h"
#include "serial_protocol.h"
#include "ui.h"

namespace {
m5can::CanMonitor g_can;
m5can::BleElmTransport g_ble;
m5can::SerialProtocol g_serial(g_can, &g_ble);
m5can::Ui g_ui;
bool g_can_ok = false;
bool g_ble_ok = false;
}

void setup() {
  g_ui.begin();
  g_serial.begin();

  g_ble_ok = g_ble.begin();
  if (!g_ble_ok) {
    Serial.printf("#WARN,BLE_INIT,%s\n", g_ble.lastError());
  } else {
    Serial.println("#BLE,ADVERTISING,M5CAN-Dial");
  }

  if (!g_can.begin()) {
    Serial.printf("#FATAL,CAN_INIT,%s\n", g_can.lastError());
    g_ui.showFatal("CAN INIT FAIL", g_can.lastError());
    return;
  }

  g_can_ok = true;
  Serial.println("#READY,Phase 3D BLE ELM + ISO-TP; TX lease required");
}

void loop() {
  // Always service the touchscreen, not only on the 250ms display redraw.
  // Only the highlighted DEMO soft key generates this one-shot event.
  if (g_can_ok && g_ui.pollDemoToggle()) {
    g_serial.toggleDemoAuto();
  }
  g_serial.pollInput();
  if (!g_can_ok) {
    delay(20);
    return;
  }

  m5can::CapturedFrame frame{};
  for (size_t i = 0; i < 64 && g_can.pop(frame, 0); ++i) {
    g_serial.emitFrame(frame);
  }

  g_serial.emitStats(false);
#if defined(M5CAN_BLE_MTU_EXPERIMENTAL) || defined(M5CAN_BLE_NOTIFY_DIAG)
  // Bench-only USB telemetry: counters are queued notifications, not peer ACKs.
  static uint32_t last_ble_diag_ms = 0;
  const uint32_t now = millis();
  if (now - last_ble_diag_ms >= 5000) {
    last_ble_diag_ms = now;
    const m5can::BleStats b = g_ble.stats();
    Serial.printf(
        "#BLE_MTU_BENCH,mode=%s,mtu=%u,max_chunk=%u,notifications=%lu,tx_bytes=%llu,rx_overflow=%lu\n",
#if defined(M5CAN_BLE_MTU_EXPERIMENTAL)
        "MTU247",
#else
        "MTU23",
#endif
        static_cast<unsigned>(b.peer_mtu),
        static_cast<unsigned>(b.max_notify_chunk),
        static_cast<unsigned long>(b.tx_notifications),
        static_cast<unsigned long long>(b.tx_bytes),
        static_cast<unsigned long>(b.rx_overflow));
  }
#endif
  g_ui.update(g_can,g_ble_ok,g_ble.connected(),g_serial.elmOverBle(),
              g_serial.demoAutoEnabled(),g_serial.demoActive());
  delay(1);
}
