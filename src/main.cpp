#include <Arduino.h>
#include <atomic>

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
TaskHandle_t g_ble_app_task=nullptr;
std::atomic<uint32_t> g_ble_poll_peak_us{0};
void bleAppTask(void*) {
  Serial.printf("#CORE,task=ble-elm,core=%d,priority=%u\n",
                xPortGetCoreID(),static_cast<unsigned>(uxTaskPriorityGet(nullptr)));
  for (;;) {
    const uint32_t start_us=micros();
    g_dual.poll();
    const uint32_t elapsed_us=micros()-start_us;
    uint32_t current=g_ble_poll_peak_us.load(std::memory_order_relaxed);
    if (elapsed_us>current)
      g_ble_poll_peak_us.store(elapsed_us,std::memory_order_relaxed);
    vTaskDelay(pdMS_TO_TICKS(1)>0?pdMS_TO_TICKS(1):1);
  }
}
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
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  if (xTaskCreatePinnedToCore(bleAppTask,"m5can-ble-elm",8192,
          nullptr,4,&g_ble_app_task,M5CAN_DUAL_BLE_APP_CORE)!=pdPASS) {
    Serial.println("#FATAL,BLE_TASK_CREATE");
    g_ui.showFatal("BLE TASK FAIL","core1 unavailable");
    g_can_ok=false;
    return;
  }
  Serial.printf("#CORE,task=arduino-ui,core=%d,can-owner-core=0,ble-app-core=1\n",
                xPortGetCoreID());
#endif
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
  // BLE worker owns DualBleElm; do not poll sessions from the LCD loop.
  static uint32_t last_dual_stats=0;
  if (millis()-last_dual_stats>=5000) {
    last_dual_stats=millis();
    const auto stats=g_ble.stats();
    Serial.printf("#BLE_DUAL,clients=%u,rx_overflow=%lu,notify=%lu,retries=%lu,failed=%lu\n",
        g_ble.connectedClientCount(),static_cast<unsigned long>(stats.rx_overflow),
        static_cast<unsigned long>(stats.targeted_notifications),
        static_cast<unsigned long>(stats.notify_retries),static_cast<unsigned long>(stats.notify_failures));
    Serial.printf("#BLE_TASK,core=%d,poll_peak_us=%lu,stack_free_words=%u\n",
        M5CAN_DUAL_BLE_APP_CORE,
        static_cast<unsigned long>(g_ble_poll_peak_us.exchange(0)),
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(g_ble_app_task)));
    const auto peers=g_ble.peerTraffic();
    for (unsigned i=0;i<2;++i) {
      const auto& p=peers.peers[i];
      Serial.printf("#BLE_PEER,slot=%u,up=%u,id=%u,gen=%lu,addr=%04X,mtu=%u,rx_bytes=%llu,tx_bytes=%llu,rx_writes=%lu,notify=%lu,retries=%lu,failed=%lu\n",
          i+1,p.connected?1U:0U,static_cast<unsigned>(p.connection_id),
          static_cast<unsigned long>(p.generation),static_cast<unsigned>(p.address_suffix),
          static_cast<unsigned>(p.mtu),static_cast<unsigned long long>(p.rx_bytes),
          static_cast<unsigned long long>(p.tx_bytes),
          static_cast<unsigned long>(p.rx_writes),
          static_cast<unsigned long>(p.notifications),
          static_cast<unsigned long>(p.notify_retries),
          static_cast<unsigned long>(p.notify_failures));
    }
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
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  const auto peers=g_ble.peerTraffic();
  g_ui.update(g_can,g_ble_ok,g_ble.connected(),g_serial.elmOverBle(),
              g_serial.demoAutoEnabled(),g_serial.demoActive(),&peers);
#else
  g_ui.update(g_can,g_ble_ok,g_ble.connected(),g_serial.elmOverBle(),
              g_serial.demoAutoEnabled(),g_serial.demoActive());
#endif
  delay(1);
}
