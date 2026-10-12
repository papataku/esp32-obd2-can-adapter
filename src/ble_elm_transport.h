#pragma once

#include <Arduino.h>

#include <cstdint>

class BLEAdvertising;
class BLEServer;
class NimBLEServer;
class NimBLECharacteristic;
class NimBLEAdvertising;
class BLECharacteristic;

namespace m5can {

class M5CanBleServerCallbacks;
class M5CanBleRxCallbacks;

struct BleStats {
  uint64_t rx_bytes = 0;
  uint64_t tx_bytes = 0;
  uint32_t rx_overflow = 0;
  uint32_t connect_count=0,disconnect_count=0,last_connect_ms=0,last_disconnect_ms=0;
  uint16_t last_connect_conn_id=0,last_disconnect_conn_id=0;
  int32_t last_disconnect_reason=0;
  bool last_disconnect_reason_known=false;
  uint32_t tx_notifications=0;
  uint16_t peer_mtu=23,max_notify_chunk=0;
  uint32_t targeted_notifications = 0;
  uint32_t session_rejections = 0;
  uint32_t notify_retries = 0;
  uint32_t notify_failures = 0;
  uint32_t mbuf_alloc_failures=0,notify_api_failures=0;
  uint32_t memory_error_ms=0;
  uint16_t memory_error_conn=0,msys_min_free=UINT16_MAX;
  int32_t memory_error_code=0;
};


// Data from GATT callbacks is consumed by the BLE application worker, never parsed on
// the BLE host callback task. A reconnect receives a new local generation.
// Counters reflect bytes accepted from GATT writes and successfully queued
// unicast notify bytes, NOT BLE air-time or acknowledged radio delivery.
// The slot and generation disambiguate reconnections. Address suffix helps
// operators map SmartRing vs JC without guessing by connection order.
struct BlePeerTraffic {
  bool connected = false;
  uint16_t connection_id = 0;
  uint32_t generation = 0;
  uint16_t address_suffix = 0;
  uint16_t mtu = 23;
  uint64_t rx_bytes = 0;
  uint64_t tx_bytes = 0;
  uint32_t rx_writes = 0;
  uint32_t notifications = 0;
  uint32_t notify_retries = 0;
  uint32_t notify_failures = 0;
  uint32_t last_rx_ms = 0;
  uint32_t last_tx_ms = 0;
};
struct BlePeersSnapshot {
  BlePeerTraffic peers[2]{};
};

struct BleDiagnosticEvent {
  enum class Kind : uint8_t { Connect, Disconnect, Reject, Error };
  Kind kind = Kind::Connect;
  uint32_t sequence = 0, uptime_ms = 0, generation = 0;
  uint16_t connection_id = 0, address_suffix = 0, mtu = 23;
  uint16_t interval = 0, latency = 0, supervision = 0;
  int reason = 0;
  bool reason_known = false;
  uint16_t error_code = 0;
  uint32_t error_detail = 0;
  uint32_t retries = 0, failed = 0;
  uint64_t rx_bytes = 0, tx_bytes = 0;
};

struct BleClientEvent {
  enum class Type : uint8_t { Connect, Disconnect, Byte };
  Type type = Type::Byte;
  uint16_t connection_id = 0;
  uint32_t generation = 0;
  uint8_t byte = 0;
};

class BleElmTransport {
 public:
  bool begin();
  int available() const;
  int read();
  size_t write(const uint8_t* data, size_t length);

  bool connected() const { return connected_; }
  bool takeDisconnectEvent();
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  bool pollClientEvent(BleClientEvent& event);
  bool isClientConnected(uint16_t connection_id, uint32_t generation) const;
  size_t writeTo(uint16_t connection_id, uint32_t generation,
                 const uint8_t* data, size_t length);
  uint8_t connectedClientCount() const;
  BlePeersSnapshot peerTraffic() const;
  bool pollDiagnostic(BleDiagnosticEvent& event);
  uint32_t diagnosticDrops() const;
  void recordDiagnosticError(uint16_t connection_id,uint32_t generation,
                             uint16_t code,uint32_t detail=0);
#endif
  BleStats stats() const;
  const char* lastError() const { return last_error_; }

 private:
  friend class M5CanBleServerCallbacks;
  friend class M5CanBleRxCallbacks;

  void onConnect();
  void onDisconnect();
  void onConnectDetails(uint16_t conn_id);
  void onDisconnectDetails(uint16_t conn_id,uint16_t reason);
  void onLegacyMtu(uint16_t mtu);
  void onWrite(const uint8_t* data, size_t length);
  void restartAdvertising();
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  void onPeerConnect(uint16_t connection_id, uint16_t address_suffix = 0);
  void onPeerDisconnect(uint16_t connection_id);
  void onPeerWrite(uint16_t connection_id, const uint8_t* data, size_t length);
  void onPeerMtu(uint16_t connection_id, uint16_t mtu);
  void recordGapDisconnect(uint16_t connection_id, int reason,
                           uint16_t interval, uint16_t latency,
                           uint16_t supervision);
  void pushDiagnostic(BleDiagnosticEvent::Kind kind, uint16_t connection_id,
                      int reason = 0, bool known = false,
                      uint16_t interval = 0, uint16_t latency = 0,
                      uint16_t supervision = 0,
                      uint16_t code=0,uint32_t detail=0);
  void pushEvent(BleClientEvent::Type type, uint16_t connection_id,
                 uint32_t generation, uint8_t byte);
  struct Client {
    uint16_t connection_id = 0;
    uint16_t mtu = 23;
    uint32_t generation = 0;
    bool active = false;
    bool gap_disconnect_seen = false;
    BlePeerTraffic traffic{};
  };
  static constexpr size_t kMaxClients = 2;
  static constexpr size_t kEventBufferSize = 2048;
  Client clients_[kMaxClients]{};
  BleClientEvent events_[kEventBufferSize]{};
  size_t event_head_ = 0;
  size_t event_tail_ = 0;
  uint32_t next_generation_ = 0;
  static constexpr size_t kDiagnosticSlots = 64;
  BleDiagnosticEvent diagnostics_[kDiagnosticSlots]{};
  size_t diagnostic_head_ = 0, diagnostic_tail_ = 0;
  uint32_t diagnostic_seq_ = 0, diagnostic_drops_ = 0;
  NimBLEServer* server_ = nullptr;
#endif
  void setError(const char* message);

  static constexpr size_t kRxBufferSize = 512;
  uint8_t rx_buffer_[kRxBufferSize]{};
  size_t rx_head_ = 0;
  size_t rx_tail_ = 0;

  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  volatile bool connected_ = false;
  volatile bool disconnect_event_ = false;
  BleStats stats_{};
  uint16_t peer_mtu_=23;

#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  NimBLECharacteristic* tx_characteristic_ = nullptr;
  NimBLEAdvertising* advertising_ = nullptr;
#else
  BLECharacteristic* tx_characteristic_ = nullptr;
  BLEAdvertising* advertising_ = nullptr;
#endif
  char last_error_[96] = "not started";
};

}  // namespace m5can
