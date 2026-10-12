#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
struct ble_gap_conn_desc {
  uint16_t conn_handle = 0;
  uint16_t conn_itvl=24, conn_latency=0, supervision_timeout=200;
  struct { uint8_t val[6]{}; } peer_ota_addr{};
};
inline constexpr int BLE_GAP_EVENT_DISCONNECT=1;
struct ble_gap_event {
  int type=BLE_GAP_EVENT_DISCONNECT;
  struct { int reason=0; ble_gap_conn_desc conn; } disconnect;
};
struct os_mbuf { size_t length=0; std::vector<uint8_t> data; };
struct TestNotify {uint16_t conn; size_t length;std::vector<uint8_t> data;};
inline std::vector<TestNotify> test_notifications;
inline int test_notify_busy=0;
inline int os_msys_num_free(){return 12;}
inline std::vector<uint16_t> test_disconnect_requests;
inline constexpr int BLE_HS_ENOMEM=6, BLE_HS_EAGAIN=1, BLE_HS_EBUSY=15;
inline os_mbuf* ble_hs_mbuf_from_flat(const void* data,size_t length) {
  static os_mbuf buffer;
  buffer.length=length;
  const auto* bytes=static_cast<const uint8_t*>(data);
  buffer.data.assign(bytes,bytes+length);
  return &buffer;
}
inline int ble_gattc_notify_custom(uint16_t conn,uint16_t,os_mbuf* packet) { if(test_notify_busy>0){--test_notify_busy;return BLE_HS_ENOMEM;} test_notifications.push_back({conn,packet->length,packet->data}); return 0; }
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
  inline static NimBLECharacteristicCallbacks* callbacks=nullptr;
  inline static std::string next_write_value;
  void setCallbacks(NimBLECharacteristicCallbacks* c) {callbacks=c;}
  uint16_t getHandle() const {return 1;}
  std::string getValue() const {return next_write_value;}
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
  inline static NimBLEServerCallbacks* callbacks=nullptr;
  void setCallbacks(NimBLEServerCallbacks* c) {callbacks=c;}
  NimBLEService* createService(const char*) {
    static NimBLEService s;
    return &s;
  }
  int disconnect(uint16_t conn){test_disconnect_requests.push_back(conn);return 0;}
};
class NimBLEDevice {
 public:
  inline static int (*gap_handler)(ble_gap_event*,void*) = nullptr;
  static void setCustomGapHandler(int (*handler)(ble_gap_event*,void*)) {
    gap_handler=handler;
  }
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
