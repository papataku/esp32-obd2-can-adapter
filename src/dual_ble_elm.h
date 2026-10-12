#pragma once
#include <Arduino.h>
#include "ble_elm_transport.h"
#include "can_monitor.h"
#include "elm_compat.h"
#include "demo_drive.h"
#include "adaptive_multi_pid.h"

namespace m5can {
class SerialProtocol;

// Experimental two-client BLE ELM front-end. All state lives in this class,
// which is polled from the Arduino loop. CAN continues on the existing
// independent TWAI owner task, and at most one query is in flight.
class DualBleElm {
 public:
  DualBleElm(CanMonitor& can, BleElmTransport& ble,
             SerialProtocol& demo_source)
      : can_(can), ble_(ble), demo_source_(demo_source) {}
  void poll();

 private:
  struct Session {
    bool active = false;
    uint16_t conn_id = 0;
    uint32_t generation = 0;
    ElmCompat elm{};
    char input[129]{};
    size_t input_size = 0;
    bool input_overflow = false;
    uint8_t output[512]{};
    size_t output_size = 0;
    bool pending = false;
    bool batch = false;
    bool multi_pid_pair = false;
    uint8_t batch_next = 0;
    uint8_t batch_count = 0;
    ElmVehicleRequest items[ElmResult::kBatchMaxIDs]{};
    DiagnosticRequest request{};
  };
  void event(const BleClientEvent& input);
  void onLine(Session& session, const char* line);
  void emit(Session& session, const char* text);
  void emitBytes(Session& session, const uint8_t* data, size_t length);
  void emitLine(Session& session, const char* text);
  void prompt(Session& session);
  void flush(Session& session);
  void appendFrame(Session& session, const DiagnosticRawFrame& frame);
  void appendResult(Session& session, const DiagnosticResult& result);
  void advanceBatch(Session& session);
  void dispatch();
  void onResult();
  Session* find(uint16_t conn_id, uint32_t generation);
  static DiagnosticRequest requestOf(const ElmVehicleRequest& item,
                                     const ElmCompat& elm);

  AdaptiveMultiPid adaptive_multi_pid_{};
  CanMonitor& can_;
  BleElmTransport& ble_;
  SerialProtocol& demo_source_;
  Session sessions_[2]{};
  int active_query_owner_ = -1;
  uint32_t active_query_generation_ = 0;
  uint8_t round_robin_ = 0;
};
} // namespace m5can
