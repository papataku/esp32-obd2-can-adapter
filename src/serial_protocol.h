#pragma once

#include <Arduino.h>

#include "ble_elm_transport.h"
#include "can_monitor.h"
#include "elm_compat.h"
#include "native_protocol.h"

namespace m5can {

class SerialProtocol {
 public:
  explicit SerialProtocol(CanMonitor& monitor,
                          BleElmTransport* ble = nullptr)
      : monitor_(monitor), ble_(ble) {}

  void begin();
  void pollInput();
  void emitFrame(const CapturedFrame& frame);
  void emitStats(bool forced = false);

  bool elmMode() const { return elm_mode_; }
  bool elmOverBle() const {
    return elm_mode_ && elm_link_ == ElmLink::Ble;
  }

 private:
  enum class ElmLink : uint8_t { None, Usb, Ble };

  struct LineBuffer {
    char data[129]{};
    size_t len = 0;
    bool overflow = false;
  };

  void emitTextHello();
  void emitTextFrame(const CapturedFrame& frame);
  void handleUsbLine(const char* line);

  bool claimElm(ElmLink link);
  void leaveElmMode();
  void handleElmLine(const char* line, ElmLink link);
  void writeElmBytes(const uint8_t* data, size_t length);
  void writeElmLine(const char* text);
  void writeElmPrompt();
  void writeElmRawFrame(const DiagnosticRawFrame& frame);
  void writeElmDiagnosticResult(const DiagnosticResult& result);
  bool looksLikeElmCommand(const char* text) const;

  void pollUsbInput();
  void pollBleInput();
  void processUsbByte(uint8_t byte);
  void processBleByte(uint8_t byte);
  void resetLine(LineBuffer& line);

  void enterNativeMode();
  void leaveNativeMode();
  void processNativeByte(uint8_t byte);
  void handleNativePacket(const native::DecodedPacket& packet);
  void emitNativePacket(native::PacketType type, const uint8_t* payload,
                        uint16_t payload_len, uint8_t flags = 0);
  void emitNativeHello();
  void emitNativeStats();
  void emitNativeEvent(const char* text);
  void emitNativeReply(native::Command command, native::Status status);
  void emitNativeFrame(const CapturedFrame& frame);

  void handleBleDisconnect();
  void writeBleBusy();
  const char* stateName(twai_state_t state) const;

  CanMonitor& monitor_;
  BleElmTransport* ble_ = nullptr;
  ElmCompat elm_{};
  ElmLink elm_link_ = ElmLink::None;
  bool elm_mode_ = false;
  bool elm_query_pending_ = false;
  bool native_mode_ = false;
  bool stream_enabled_ = true;
  uint16_t native_tx_sequence_ = 0;
  uint64_t native_rx_errors_ = 0;
  uint32_t last_stats_ms_ = 0;

  LineBuffer usb_line_{};
  LineBuffer ble_line_{};

  uint8_t native_input_[cfg::kNativeMaxEncodedPacket]{};
  size_t native_input_len_ = 0;
  bool native_discard_until_delimiter_ = false;
};

}  // namespace m5can
