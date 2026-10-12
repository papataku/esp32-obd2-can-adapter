#include <Arduino.h>
#if __has_include(<esp_system.h>)
#include <esp_system.h>
#define M5CAN_HAS_ESP_SYSTEM 1
#else
#define M5CAN_HAS_ESP_SYSTEM 0
#endif

#include "ble_elm_transport.h"
#include "can_monitor.h"
#include "serial_protocol.h"
#include "ui.h"

namespace {
// Host-syntax tests do not ship Espressif runtime headers; the device build
// records the real reset reason and heap instead of masking host build errors.
int bootResetReason() {
#if M5CAN_HAS_ESP_SYSTEM
  return static_cast<int>(esp_reset_reason());
#else
  return -1;
#endif
}
uint32_t freeHeapBytes() {
#if M5CAN_HAS_ESP_SYSTEM
  return ESP.getFreeHeap();
#else
  return 0;
#endif
}
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
  Serial.printf("#BOOT,reset_reason=%d,uptime_ms=%lu,free_heap=%lu\n",
                bootResetReason(),
                static_cast<unsigned long>(millis()),
                static_cast<unsigned long>(freeHeapBytes()));

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
  if (g_ble_ok) {
    // Emit callback events from loop(), never from the BLE stack callback.
    // Match these uptime values against iPad BLE_STATE session timestamps.
    static uint32_t seen_connects = 0;
    static uint32_t seen_disconnects = 0;
    const auto stats = g_ble.stats();
    if (stats.connect_count != seen_connects) {
      seen_connects = stats.connect_count;
      Serial.printf("#BLE,CONNECT,count=%lu,conn_id=%u,uptime_ms=%lu,free_heap=%lu\n",
                    static_cast<unsigned long>(seen_connects),
                    static_cast<unsigned>(stats.last_connect_conn_id),
                    static_cast<unsigned long>(stats.last_connect_ms),
                    static_cast<unsigned long>(freeHeapBytes()));
    }
    if (stats.disconnect_count != seen_disconnects) {
      seen_disconnects = stats.disconnect_count;
      Serial.printf("#BLE,DISCONNECT,count=%lu,conn_id=%u,reason_known=%u,reason=0x%02X,uptime_ms=%lu,free_heap=%lu\n",
                    static_cast<unsigned long>(seen_disconnects),
                    static_cast<unsigned>(stats.last_disconnect_conn_id),
                    stats.last_disconnect_reason_known ? 1U : 0U,
                    static_cast<unsigned>(stats.last_disconnect_reason),
                    static_cast<unsigned long>(stats.last_disconnect_ms),
                    static_cast<unsigned long>(freeHeapBytes()));
    }
  }
  if (!g_can_ok) {
    delay(20);
    return;
  }

  m5can::CapturedFrame frame{};
  for (size_t i = 0; i < 64 && g_can.pop(frame, 0); ++i) {
    g_serial.emitFrame(frame);
  }

  g_serial.emitStats(false);
  g_ui.update(g_can,g_ble.stats(),g_ble_ok,g_ble.connected(),g_serial.elmOverBle(),
              g_serial.demoAutoEnabled(),g_serial.demoActive());
  delay(1);
}
