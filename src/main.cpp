#include <Arduino.h>

#include "can_monitor.h"
#include "serial_protocol.h"
#include "ui.h"

namespace {
m5can::CanMonitor g_can;
m5can::SerialProtocol g_serial(g_can);
m5can::Ui g_ui;
bool g_can_ok = false;
}

void setup() {
  g_ui.begin();
  g_serial.begin();

  if (!g_can.begin()) {
    Serial.printf("#FATAL,CAN_INIT,%s\n", g_can.lastError());
    g_ui.showFatal("CAN INIT FAIL", g_can.lastError());
    return;
  }

  g_can_ok = true;
  Serial.println("#READY,Phase 3B ELM read-only TX capability; lease required");
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
  g_ui.update(g_can);
  delay(1);
}
