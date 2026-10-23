#include "evidence_store.h"
#include <Arduino.h>
#include <atomic>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <rom/rtc.h>
#endif

#include "ble_elm_transport.h"
#include "can_monitor.h"
#if defined(M5CAN_RAW_CAN_LOG_RUNTIME)
#include "raw_can_service.h"
#endif
#include "serial_protocol.h"
#include "ui.h"
#ifndef M5CAN_BUILD_SHA
#define M5CAN_BUILD_SHA "unknown"
#endif
#include "drive_log.h"
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
#include "dual_ble_elm.h"
#include "ble_evidence_journal.h"
#ifndef M5CAN_DUAL_BLE_APP_CORE
#define M5CAN_DUAL_BLE_APP_CORE 1
#endif
#endif

namespace {
#if !defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
uint32_t bootFreeHeapBytes() {
#if defined(ARDUINO_ARCH_ESP32)
  return ESP.getFreeHeap();
#else
  return 0;
#endif
}
#endif
m5can::CanMonitor g_can;
#if defined(M5CAN_RAW_CAN_LOG_RUNTIME)
m5can::RawCanRecordingService g_raw(g_can);
#endif
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
  g_serial.setDriveLogTransport(&g_ble);
#if defined(M5CAN_RAW_CAN_LOG_RUNTIME)
  g_serial.setRawCanRecorder(&g_raw);
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  g_dual.setRawCanRecorder(&g_raw);
#endif
#endif
  g_ui.begin();
  g_serial.begin();
#if defined(M5CAN_RAW_CAN_LOG_RUNTIME)
  // Must run before starting TWAI/BLE. If the proposed 'canlog' partition
  // is absent or still contains old OTA bytes, this fails closed.
  const bool raw_ready=g_raw.begin();
  Serial.printf("#CANLOG,boot_ready=%u,reason=%s,blocks=%lu,foreign=%u\n",
      raw_ready?1U:0U,raw_ready?"OK":"ABSENT_OR_UNOWNED",
      static_cast<unsigned long>(m5can::rawCanJournal().status().blocks),
      m5can::rawCanJournal().status().foreign?1U:0U);
#endif
#if defined(ARDUINO_ARCH_ESP32)
  m5can::driveLog().begin(static_cast<uint32_t>(esp_reset_reason()),
      static_cast<uint32_t>(rtc_get_reset_reason(0)));
#else
  m5can::driveLog().begin(0);
#endif
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  const bool log_store_ready=m5can::initializeEvidenceStore();
  Serial.printf("#LOGSTORE,ready=%u,sectors=%lu,foreign=%u\n",log_store_ready?1U:0U,
      static_cast<unsigned long>(m5can::evidenceStore().status().sectors),
      m5can::evidenceStore().status().foreign?1U:0U);
  m5can::bleEvidenceJournal().begin(m5can::driveLog().bootSequence(),
                                    M5CAN_BUILD_SHA);
#endif

  g_ble_ok = g_ble.begin();
#if defined(ARDUINO_ARCH_ESP32)
  Serial.printf("#BOOT,git=%s,reset=%u,rom_reset=%u,stack=NimBLE_dual_if_enabled\n",
      M5CAN_BUILD_SHA, static_cast<unsigned>(esp_reset_reason()),
      static_cast<unsigned>(rtc_get_reset_reason(0)));
#endif
#if defined(M5CAN_LOG_STREAM_V2_EXPERIMENTAL) && defined(M5CAN_RAW_CAN_LOG_RUNTIME)
  // Actual readiness requires all three: registered dedicated GATT service,
  // allowlisted bonded peer and an already-safe committed CANLOG session.
  // Never print the approved peer ID or call compiled==ready.
  unsigned allowlisted=0;
#if defined(M5CAN_LOG_V2_ALLOWED_PEER_ID48)
  allowlisted=1;
#endif
  Serial.printf("#V2,compiled=1,gatt_init=%u,peer_allowlisted=%u,canlog_ready=%u,blocks=%lu,window=16,mtu_req=247\n",
      g_ble_ok?1U:0U,allowlisted,g_raw.ready()?1U:0U,
      static_cast<unsigned long>(m5can::rawCanJournal().status().blocks));
#endif
  if (!g_ble_ok) {
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
    m5can::bleEvidenceJournal().appendError(m5can::DiagnosticError::BleInitFailure);
#endif
    Serial.printf("#WARN,BLE_INIT,%s\n", g_ble.lastError());
  } else {
    Serial.println("#BLE,ADVERTISING,M5CAN-Dial");
  }

  if (!g_can.begin()) {
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
    m5can::bleEvidenceJournal().appendError(m5can::DiagnosticError::CanInitFailed);
#endif
    Serial.printf("#FATAL,CAN_INIT,%s\n", g_can.lastError());
    g_ui.showFatal("CAN INIT FAIL", g_can.lastError());
    return;
  }

  g_can_ok = true;
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  if (xTaskCreatePinnedToCore(bleAppTask,"m5can-ble-elm",8192,
          nullptr,4,&g_ble_app_task,M5CAN_DUAL_BLE_APP_CORE)!=pdPASS) {
    m5can::bleEvidenceJournal().appendError(
        m5can::DiagnosticError::AppTaskCreateFailed);
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
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  m5can::BleDiagnosticEvent event{};
  m5can::bleEvidenceJournal().observeQueueDrops(g_ble.diagnosticDrops());
  for (unsigned n=0;n<8 && g_ble.pollDiagnostic(event);++n) {
    m5can::bleEvidenceJournal().append(event,g_ble.diagnosticDrops());
    // Long CDC serial writes during a dual-peer session compete with
    // the Arduino loop and can skew the very latencies being diagnosed.
    // Persist complete bounded evidence now; dump it after the run.
    if(g_ble.connectedClientCount()!=0 || g_can.queryPending())continue;
    const char* kind=event.kind==m5can::BleDiagnosticEvent::Kind::Connect ? "CONNECT"
        : event.kind==m5can::BleDiagnosticEvent::Kind::Disconnect ? "DISCONNECT"
        : event.kind==m5can::BleDiagnosticEvent::Kind::Reject ? "REJECT" : "ERROR";
    Serial.printf("#BLE_EVENT,kind=%s,seq=%lu,ms=%lu,id=%u,gen=%lu,addr=%04X,reason_known=%u,reason=%d,mtu=%u,interval=%u,latency=%u,supervision=%u,rx=%llu,tx=%llu,retries=%lu,failed=%lu,drops=%lu,code=%u,error=%s,detail=%lu\n",
        kind,static_cast<unsigned long>(event.sequence),static_cast<unsigned long>(event.uptime_ms),
        event.connection_id,static_cast<unsigned long>(event.generation),event.address_suffix,
        event.reason_known?1U:0U,event.reason,event.mtu,event.interval,event.latency,event.supervision,
        static_cast<unsigned long long>(event.rx_bytes),static_cast<unsigned long long>(event.tx_bytes),
        static_cast<unsigned long>(event.retries),static_cast<unsigned long>(event.failed),
        static_cast<unsigned long>(g_ble.diagnosticDrops()),
        event.error_code,m5can::diagnosticErrorName(event.error_code),
        static_cast<unsigned long>(event.error_detail));
  }

  static uint32_t last_loop_us=0,loop_peak_us=0;
  const uint32_t loop_now=micros();
  if (last_loop_us && loop_now-last_loop_us>loop_peak_us)
    loop_peak_us=loop_now-last_loop_us;
  last_loop_us=loop_now;
#endif
  // Always service the touchscreen, not only on the 250ms display redraw.
  // Only the highlighted DEMO soft key generates this one-shot event.
  if (g_can_ok && g_ui.pollDemoToggle()) {
    g_serial.toggleDemoAuto();
  }
  g_serial.pollInput();
#if defined(M5CAN_RAW_CAN_LOG_RUNTIME)
  if(g_can_ok)g_raw.poll(); // bounded consumer, only Arduino CPU1 writes flash
#endif
#if !defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
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
                    static_cast<unsigned long>(bootFreeHeapBytes()));
    }
    if (stats.disconnect_count != seen_disconnects) {
      seen_disconnects = stats.disconnect_count;
      Serial.printf("#BLE,DISCONNECT,count=%lu,conn_id=%u,reason_known=%u,reason=0x%02X,uptime_ms=%lu,free_heap=%lu\n",
                    static_cast<unsigned long>(seen_disconnects),
                    static_cast<unsigned>(stats.last_disconnect_conn_id),
                    stats.last_disconnect_reason_known ? 1U : 0U,
                    static_cast<unsigned>(stats.last_disconnect_reason),
                    static_cast<unsigned long>(stats.last_disconnect_ms),
                    static_cast<unsigned long>(bootFreeHeapBytes()));
    }
  }
#endif

#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  // BLE worker owns DualBleElm; do not poll sessions from the LCD loop.
  static uint32_t last_dual_stats=0;
  if (millis()-last_dual_stats>=5000) {
    last_dual_stats=millis();
    const auto stats=g_ble.stats();
    static uint32_t seen_storage_errors=0;
    const uint32_t storage_errors=m5can::driveLog().storageErrors();
    if(storage_errors!=seen_storage_errors) {
      m5can::bleEvidenceJournal().appendError(m5can::DiagnosticError::DriveStorageFailure,storage_errors);
      seen_storage_errors=storage_errors;
    }
    // Counter snapshots catch CAN owner and GATT failures even when an
    // individual event ring overflows; compare monotonic counters.
    m5can::bleEvidenceJournal().observeHealth(g_can.stats(),stats);
    Serial.printf("#BLE_DUAL,clients=%u,connects=%lu,disconnects=%lu,rx_overflow=%lu,notify=%lu,retries=%lu,failed=%lu\n",
        g_ble.connectedClientCount(),static_cast<unsigned long>(stats.connect_count),
        static_cast<unsigned long>(stats.disconnect_count),static_cast<unsigned long>(stats.rx_overflow),
        static_cast<unsigned long>(stats.targeted_notifications),
        static_cast<unsigned long>(stats.notify_retries),static_cast<unsigned long>(stats.notify_failures));
    Serial.printf("#BLE_MEMORY,alloc_failed=%lu,api_failed=%lu,min_free=%u,last_ms=%lu,last_conn=%u,last_rc=%ld\n",
        static_cast<unsigned long>(stats.mbuf_alloc_failures),
        static_cast<unsigned long>(stats.notify_api_failures),stats.msys_min_free,
        static_cast<unsigned long>(stats.memory_error_ms),stats.memory_error_conn,
        static_cast<long>(stats.memory_error_code));
    Serial.printf("#BLE_FLOW,waits=%lu,queue_peak=%u,host_peak=%u,host_queue_peak=%u,overflow=%lu,stalls=%lu\n",
        static_cast<unsigned long>(stats.credit_waits),stats.tx_queue_peak,stats.host_pending_peak,stats.host_queue_peak,
        static_cast<unsigned long>(stats.tx_queue_overflows),static_cast<unsigned long>(stats.tx_stalls));
#if defined(M5CAN_LOG_STREAM_V2_EXPERIMENTAL)
    // Five-second snapshots, not per-notify Serial. Cumulative V2 reasons
    // distinguish ordinary ELM traffic, host credit and memory pressure.
    const auto credit=g_ble.v2CreditSnapshot();
    Serial.printf("#BLE_V2_FLOW,uptime_ms=%lu,ok=%lu,elm_wait=%lu,auth_denied=%lu,msys_wait=%lu,host_wait=%lu,mbuf_fail=%lu,notify_fail=%lu,bad=%lu\n",
        static_cast<unsigned long>(millis()),
        static_cast<unsigned long>(stats.v2_notify_ok),
        static_cast<unsigned long>(stats.v2_wait_elm),
        static_cast<unsigned long>(stats.v2_denied_link),
        static_cast<unsigned long>(stats.v2_wait_msys),
        static_cast<unsigned long>(stats.v2_wait_host),
        static_cast<unsigned long>(stats.v2_mbuf_fail),
        static_cast<unsigned long>(stats.v2_notify_api_fail),
        static_cast<unsigned long>(stats.v2_bad_frame));
    Serial.printf("#BLE_V2_CREDIT,uptime_ms=%lu,msys_free=%u,peer0=%u,ascii0=%u,outstanding0=%u,queued0=%u,peer1=%u,ascii1=%u,outstanding1=%u,queued1=%u\n",
        static_cast<unsigned long>(millis()),
        static_cast<unsigned>(credit.msys_free),
        credit.peers[0].active?1U:0U,
        static_cast<unsigned>(credit.peers[0].ascii_pending_bytes),
        static_cast<unsigned>(credit.peers[0].host_outstanding),
        static_cast<unsigned>(credit.peers[0].host_queued),
        credit.peers[1].active?1U:0U,
        static_cast<unsigned>(credit.peers[1].ascii_pending_bytes),
        static_cast<unsigned>(credit.peers[1].host_outstanding),
        static_cast<unsigned>(credit.peers[1].host_queued));
#endif
    Serial.printf("#BLE_TASK,core=%d,poll_peak_us=%lu,stack_free_bytes=%u,ui_loop_peak_us=%lu\n",
        M5CAN_DUAL_BLE_APP_CORE,
        static_cast<unsigned long>(g_ble_poll_peak_us.exchange(0)),
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(g_ble_app_task)),
        static_cast<unsigned long>(loop_peak_us));
    loop_peak_us=0;
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
    const auto coordinator=g_dual.coordinatorStats();
    // These are logical coordinator submissions; physical TX is separately
    // counted by CanMonitor::stats(). Do not label these as CAN frames.
    Serial.printf("#PID_COORD,cache_hit=%lu,cache_miss=%lu,join=%lu,group_submit=%lu,cross_group=%lu,single_submit=%lu\n",
        static_cast<unsigned long>(coordinator.cache_hits),
        static_cast<unsigned long>(coordinator.cache_misses),
        static_cast<unsigned long>(coordinator.joins),
        static_cast<unsigned long>(coordinator.grouped_submissions),
        static_cast<unsigned long>(coordinator.cross_group_submissions),
        static_cast<unsigned long>(coordinator.single_submissions));
#endif
#if defined(ARDUINO_ARCH_ESP32)
    m5can::driveLog().resources(g_can.ownerStackFreeBytes(),
        static_cast<uint32_t>(uxTaskGetStackHighWaterMark(g_ble_app_task)),
        static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    Serial.printf("#MEMORY,internal_free=%u,internal_min=%u,largest=%u,can_stack_free_bytes=%lu\n",
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned long>(g_can.ownerStackFreeBytes()));
#endif
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
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
    // Initialisation may fail before normal loop journaling is reached.
    m5can::bleEvidenceJournal().poll(g_can);
#endif
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

#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
  const auto coord_log=g_dual.coordinatorStats();
  m5can::driveLog().coordinator(coord_log.cache_hits,coord_log.cache_misses,
      coord_log.joins,coord_log.grouped_submissions,coord_log.cross_group_submissions,
      coord_log.single_submissions);
#endif
  m5can::driveLog().poll(g_can,g_ble,g_serial.demoActive());
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  // Flash only from Arduino loop and only at CAN-idle checkpoints.
  // Never block NimBLE GAP callbacks or CPU0 CAN owner with NVS writes.
  m5can::bleEvidenceJournal().poll(g_can);
#endif
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  const auto peers=g_ble.peerTraffic();
  g_ui.update(g_can,g_ble.stats(),g_ble_ok,g_ble.connected(),g_serial.elmOverBle(),
              g_serial.demoAutoEnabled(),g_serial.demoActive(),&peers);
#else
  g_ui.update(g_can,g_ble.stats(),g_ble_ok,g_ble.connected(),g_serial.elmOverBle(),
              g_serial.demoAutoEnabled(),g_serial.demoActive());
#endif
  delay(1);
}
