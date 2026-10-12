#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
struct ble_gap_conn_desc { uint16_t conn_handle = 0; };
struct os_mbuf {};
inline os_mbuf* ble_hs_mbuf_from_flat(const void*,size_t) {
  static os_mbuf buffer;
  return &buffer;
}
inline int ble_gattc_notify_custom(uint16_t,uint16_t,os_mbuf*) { return 0; }
namespace NIMBLE_PROPERTY {
  static constexpr uint32_t WRITE = 1;
  static constexpr uint32_t WRITE_NR = 2;
  static constexpr uint32_t NOTIFY = 4;
}
class NimBLEServer;
class NimBLECharacteristic;
class NimBLEServerCallbacks {
 public:
  virtual ~NimBLEServerCallbacks()=default;
  virtual void onConnect(NimBLEServer*,ble_gap_conn_desc*) {}
  virtual void onDisconnect(NimBLEServer*,ble_gap_conn_desc*) {}
  virtual void onMTUChange(uint16_t,ble_gap_conn_desc*) {}
};
class NimBLECharacteristicCallbacks {
 public:
  virtual ~NimBLECharacteristicCallbacks()=default;
  virtual void onWrite(NimBLECharacteristic*,ble_gap_conn_desc*) {}
};
class NimBLECharacteristic {
 public:
  void setCallbacks(NimBLECharacteristicCallbacks*) {}
  uint16_t getHandle() const {return 1;}
  std::string getValue() const {return "";}
};
class NimBLEService {
 public:
  NimBLECharacteristic* createCharacteristic(const char*,uint32_t) {
    static NimBLECharacteristic c;
    return &c;
  }
  void start() {}
};
class NimBLEAdvertising {
 public:
  void addServiceUUID(const char*) {}
  void setScanResponse(bool) {}
  bool start() {return true;}
};
class NimBLEServer {
 public:
  void setCallbacks(NimBLEServerCallbacks*) {}
  NimBLEService* createService(const char*) {
    static NimBLEService s;
    return &s;
  }
  int disconnect(uint16_t){return 0;}
};
class NimBLEDevice {
 public:
  static void init(const char*) {}
  static int setMTU(uint16_t){return 0;}
  static NimBLEServer* createServer() {
    static NimBLEServer s;
    return &s;
  }
  static NimBLEAdvertising* getAdvertising() {
    static NimBLEAdvertising a;
    return &a;
  }
};
