#pragma once

#include <Arduino.h>

namespace m5can {

enum class ElmAction : uint8_t {
  ReplyOnly,
  VehicleRead,
  BatchRead,
  LeaseAcquire,
  LeaseRevoke,
  LeaseStatus,
  ExitElmMode,
};

struct ElmVehicleRequest {
  uint32_t can_id = 0;
  bool extended = true;
  uint8_t length = 0;
  uint8_t data[8]{};
};

struct ElmResult {
  static constexpr uint8_t kBatchMaxIDs = 16;
  ElmAction action = ElmAction::ReplyOnly;
  char reply[160]{};
  ElmVehicleRequest request{};
  uint8_t batch_count = 0;
  ElmVehicleRequest batch[kBatchMaxIDs]{};
};

class ElmCompat {
 public:
  ElmCompat() { reset(); }

  void reset();
  ElmResult execute(const char* command);

  bool echo() const { return echo_; }
  bool linefeed() const { return linefeed_; }
  bool spaces() const { return spaces_; }
  bool headers() const { return headers_; }
  bool autoFormatting() const { return auto_format_; }
  bool flowControl() const { return flow_control_; }
  uint8_t protocol() const { return protocol_; }
  uint8_t priority() const { return priority_; }
  uint32_t effectiveHeader() const {
    return (static_cast<uint32_t>(priority_) << 24) | lower_header_;
  }
  uint8_t adaptiveTiming() const { return adaptive_timing_; }
  uint8_t timeout4ms() const { return timeout_4ms_; }

 private:
  static bool parseHexByte(const char* text, uint8_t& value);
  static bool parseHexBytes(const char* text, uint8_t* output, size_t capacity,
                            uint8_t& length);
  static void normalize(const char* input, char* output, size_t capacity);
  static bool startsWith(const char* text, const char* prefix);
  static void setReply(ElmResult& result, const char* text);
  bool validateVehicleRead(const uint8_t* bytes, uint8_t length) const;

  bool echo_ = true;
  bool linefeed_ = false;
  bool spaces_ = false;
  bool headers_ = true;
  bool allow_long_ = true;
  bool auto_format_ = true;
  bool flow_control_ = true;
  uint8_t protocol_ = 7;
  uint8_t priority_ = 0x18;
  uint32_t lower_header_ = 0xDB33F1;
  uint8_t adaptive_timing_ = 1;
  uint8_t timeout_4ms_ = 0x32;
};

}  // namespace m5can
