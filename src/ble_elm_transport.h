#pragma once

#include <Arduino.h>

class BLEAdvertising;
class BLECharacteristic;

namespace m5can {

class M5CanBleServerCallbacks;
class M5CanBleRxCallbacks;

struct BleStats {
  uint64_t rx_bytes = 0;
  uint64_t tx_bytes = 0;
  uint32_t rx_overflow = 0;
};

class BleElmTransport {
 public:
  bool begin();
  int available() const;
  int read();
  size_t write(const uint8_t* data, size_t length);

  bool connected() const { return connected_; }
  bool takeDisconnectEvent();
  BleStats stats() const;
  const char* lastError() const { return last_error_; }

 private:
  friend class M5CanBleServerCallbacks;
  friend class M5CanBleRxCallbacks;

  void onConnect();
  void onDisconnect();
  void onWrite(const uint8_t* data, size_t length);
  void restartAdvertising();
  void setError(const char* message);

  static constexpr size_t kRxBufferSize = 512;
  uint8_t rx_buffer_[kRxBufferSize]{};
  size_t rx_head_ = 0;
  size_t rx_tail_ = 0;

  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  volatile bool connected_ = false;
  volatile bool disconnect_event_ = false;
  BleStats stats_{};

  BLECharacteristic* tx_characteristic_ = nullptr;
  BLEAdvertising* advertising_ = nullptr;
  char last_error_[96] = "not started";
};

}  // namespace m5can
