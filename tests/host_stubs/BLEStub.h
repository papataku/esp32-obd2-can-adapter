#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class BLEServer;
class BLECharacteristic;

class BLEServerCallbacks {
 public:
  virtual ~BLEServerCallbacks() = default;
  virtual void onConnect(BLEServer*) {}
  virtual void onDisconnect(BLEServer*) {}
};

class BLECharacteristicCallbacks {
 public:
  virtual ~BLECharacteristicCallbacks() = default;
  virtual void onWrite(BLECharacteristic*) {}
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
  void setCallbacks(BLEServerCallbacks*) {}
  BLEService* createService(const char*) {
    static BLEService service;
    return &service;
  }
};

class BLEDevice {
 public:
  static void init(const char*) {}
  static BLEServer* createServer() {
    static BLEServer server;
    return &server;
  }
  static BLEAdvertising* getAdvertising() {
    static BLEAdvertising advertising;
    return &advertising;
  }
};
