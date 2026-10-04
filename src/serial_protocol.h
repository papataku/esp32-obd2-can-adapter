#pragma once

#include <Arduino.h>

#include "can_monitor.h"
#include "elm_compat.h"
#include "native_protocol.h"

namespace m5can {

class SerialProtocol {
 public:
  explicit SerialProtocol(CanMonitor& monitor) : monitor_(monitor) {}

  void begin();
  void pollInput();
  void emitFrame(const CapturedFrame& frame);
  void emitStats(bool forced = false);
  bool elmMode() const { return elm_mode_; }

 private:
  void emitTextHello();
  void emitTextFrame(const CapturedFrame& frame);
  void handleTextLine();

  void enterElmMode();
  void leaveElmMode();
  void handleElmLine();
  void writeElmLine(const char* text);
  void writeElmPrompt();
  void writeElmRawFrame(const DiagnosticRawFrame& frame);
  void writeElmDiagnosticResult(const DiagnosticResult& result);
  bool looksLikeElmCommand(const char* text) const;

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

  const char* stateName(twai_state_t state) const;

  CanMonitor& monitor_;
  ElmCompat elm_{};
  bool elm_mode_ = false;
  bool elm_query_pending_ = false;
  bool native_mode_ = false;
  bool stream_enabled_ = true;
  uint16_t native_tx_sequence_ = 0;
  uint64_t native_rx_errors_ = 0;
  uint32_t last_stats_ms_ = 0;

  char text_input_[129]{};
  size_t text_input_len_ = 0;
  bool text_overflow_ = false;

  uint8_t native_input_[cfg::kNativeMaxEncodedPacket]{};
  size_t native_input_len_ = 0;
  bool native_discard_until_delimiter_ = false;
};

}  // namespace m5can
