#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include "esp_gatts_api.h"

struct esp_ble_gatts_cb_param_t { struct { uint16_t mtu = 23; } mtu; };

class BLEServer;
class BLECharacteristic;

class BLEServerCallbacks {
 public:
  virtual ~BLEServerCallbacks() = default;
  virtual void onConnect(BLEServer*) {}
  virtual void onConnect(BLEServer*, esp_ble_gatts_cb_param_t*) {}
  virtual void onDisconnect(BLEServer*) {}
  virtual void onDisconnect(BLEServer*, esp_ble_gatts_cb_param_t*) {}
  virtual void onMtuChanged(BLEServer*, esp_ble_gatts_cb_param_t*) {}
};

class BLECharacteristicCallbacks {
 public:
  virtual ~BLECharacteristicCallbacks() = default;
  virtual void onWrite(BLECharacteristic*) {}
  virtual void onWrite(BLECharacteristic*, esp_ble_gatts_cb_param_t*) {}
};

class BLEDescriptor {
 public:
  virtual ~BLEDescriptor() = default;
};

class BLE2902 : public BLEDescriptor {};

class BLECharacteristic {
 public:
  static constexpr uint32_t PROPERTY_WRITE = 1U << 0;
  static constexpr uint32_t PROPERTY_WRITE_NR = 1U << 1;
  static constexpr uint32_t PROPERTY_NOTIFY = 1U << 2;

  void setCallbacks(BLECharacteristicCallbacks*) {}
  void addDescriptor(BLEDescriptor*) {}
  std::string getValue() const { return {}; }
  void setValue(uint8_t*, size_t) {}
  void notify() {}
  uint16_t getHandle() const { return 42; }
};

class BLEService {
 public:
  BLECharacteristic* createCharacteristic(const char*, uint32_t) {
    static BLECharacteristic characteristic;
    return &characteristic;
  }
  void start() {}
};

class BLEAdvertising {
 public:
  void addServiceUUID(const char*) {}
  void setScanResponse(bool) {}
  void start() {}
};

class BLEServer {
 public:
  void disconnect(uint16_t) {}
  uint8_t getGattsIf() const { return 0; }
  void setCallbacks(BLEServerCallbacks*) {}
  BLEService* createService(const char*) {
    static BLEService service;
    return &service;
  }
};

class BLEDevice {
 public:
  static void init(const char*) {}
  static int setMTU(uint16_t) { return 0; }
  static BLEServer* createServer() {
    static BLEServer server;
    return &server;
  }
  static BLEAdvertising* getAdvertising() {
    static BLEAdvertising advertising;
    return &advertising;
  }
};
