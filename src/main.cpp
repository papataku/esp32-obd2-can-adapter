#include <Arduino.h>

#include "ble_elm_transport.h"
#include "can_monitor.h"
#include "serial_protocol.h"
#include "ui.h"
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
#include "dual_ble_elm.h"
#endif

namespace {
m5can::CanMonitor g_can;
m5can::BleElmTransport g_ble;
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
m5can::SerialProtocol g_serial(g_can, nullptr); // USB only, separate ELM state
m5can::DualBleElm g_dual(g_can,g_ble,g_serial);
#else
m5can::SerialProtocol g_serial(g_can, &g_ble);
#endif
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
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  g_dual.poll();
  static uint32_t last_dual_stats=0;
  if (millis()-last_dual_stats>=5000) {
    last_dual_stats=millis();
    const auto stats=g_ble.stats();
    Serial.printf("#BLE_DUAL,clients=%u,rx_overflow=%lu,notify=%lu,retries=%lu,failed=%lu\n",
        g_ble.connectedClientCount(),static_cast<unsigned long>(stats.rx_overflow),
        static_cast<unsigned long>(stats.targeted_notifications),
        static_cast<unsigned long>(stats.notify_retries),static_cast<unsigned long>(stats.notify_failures));
  }
#endif
  if (!g_can_ok) {
    delay(20);
    return;
  }

  m5can::CapturedFrame frame{};
  for (size_t i = 0; i < 64 && g_can.pop(frame, 0); ++i) {
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
    // Avoid flooding blocking USB serial with CAN frames while two BLE
    // clients are polling. CAN capture and safety accounting remain active.
    if (!g_ble.connected()) g_serial.emitFrame(frame);
#else
    g_serial.emitFrame(frame);
#endif
  }

  g_serial.emitStats(false);
  g_ui.update(g_can,g_ble_ok,g_ble.connected(),g_serial.elmOverBle(),
              g_serial.demoAutoEnabled(),g_serial.demoActive());
  delay(1);
}
