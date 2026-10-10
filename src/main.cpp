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
  g_ui.update(g_can, g_ble_ok, g_ble.connected(), g_serial.elmOverBle(), g_serial.demoActive());
  delay(1);
}
