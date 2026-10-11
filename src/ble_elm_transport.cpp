#include "ble_elm_transport.h"
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
#include <NimBLEDevice.h>
// NimBLE exposes connection-aware GATT callbacks and allows sending one
// notification to exactly one connection via ble_gattc_notify_custom.
#else
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#endif
#include <algorithm>
#include <cstring>
#include <string>
#include "app_config.h"

namespace m5can {
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
class M5CanBleServerCallbacks final : public NimBLEServerCallbacks {
 public:
  explicit M5CanBleServerCallbacks(BleElmTransport& owner) : owner_(owner) {}
  void onConnect(NimBLEServer*, ble_gap_conn_desc* d) override {
    if (d) owner_.onPeerConnect(d->conn_handle);
  }
  void onDisconnect(NimBLEServer*, ble_gap_conn_desc* d) override {
    if (d) owner_.onPeerDisconnect(d->conn_handle);
  }
  void onMTUChange(uint16_t mtu, ble_gap_conn_desc* d) override {
    if (d) owner_.onPeerMtu(d->conn_handle, mtu);
  }
 private:
  BleElmTransport& owner_;
};
class M5CanBleRxCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  explicit M5CanBleRxCallbacks(BleElmTransport& owner) : owner_(owner) {}
  void onWrite(NimBLECharacteristic* characteristic,
               ble_gap_conn_desc* desc) override {
    if (!desc || !characteristic) return;
    const std::string value = characteristic->getValue();
    if (!value.empty())
      owner_.onPeerWrite(desc->conn_handle,
           reinterpret_cast<const uint8_t*>(value.data()),value.size());
  }
 private:
  BleElmTransport& owner_;
};
#else
class M5CanBleServerCallbacks final : public BLEServerCallbacks {
 public:
  explicit M5CanBleServerCallbacks(BleElmTransport& owner) : owner_(owner) {}
  void onConnect(BLEServer*) override { owner_.onConnect(); }
  void onDisconnect(BLEServer*) override { owner_.onDisconnect(); }
 private:
  BleElmTransport& owner_;
};
class M5CanBleRxCallbacks final : public BLECharacteristicCallbacks {
 public:
  explicit M5CanBleRxCallbacks(BleElmTransport& owner) : owner_(owner) {}
  void onWrite(BLECharacteristic* characteristic) override {
    const std::string value = characteristic->getValue();
    if (!value.empty())
      owner_.onWrite(reinterpret_cast<const uint8_t*>(value.data()),
                     value.size());
  }
 private:
  BleElmTransport& owner_;
};
#endif

void BleElmTransport::setError(const char* message) {
  std::strncpy(last_error_, message ? message : "unknown",
               sizeof(last_error_) - 1);
  last_error_[sizeof(last_error_) - 1] = '\0';
}

bool BleElmTransport::begin() {
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  NimBLEDevice::init(cfg::kBleDeviceName);
  NimBLEDevice::setMTU(247);
  NimBLEServer* server=NimBLEDevice::createServer();
  if (!server) { setError("NimBLE server failed"); return false; }
  server_=server;
  server->setCallbacks(new M5CanBleServerCallbacks(*this));
  NimBLEService* service=server->createService(cfg::kBleServiceUuid);
  if (!service) { setError("NimBLE service failed"); return false; }
  NimBLECharacteristic* rx=service->createCharacteristic(
      cfg::kBleRxUuid,NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  tx_characteristic_=service->createCharacteristic(
      cfg::kBleTxUuid,NIMBLE_PROPERTY::NOTIFY);
  if (!rx || !tx_characteristic_) {
    setError("NimBLE characteristic failed"); return false;
  }
  rx->setCallbacks(new M5CanBleRxCallbacks(*this));
  service->start();
  advertising_=NimBLEDevice::getAdvertising();
#else
  BLEDevice::init(cfg::kBleDeviceName);
  BLEServer* server=BLEDevice::createServer();
  if (!server) { setError("BLE server failed"); return false; }
  server->setCallbacks(new M5CanBleServerCallbacks(*this));
  BLEService* service=server->createService(cfg::kBleServiceUuid);
  if (!service) { setError("BLE service failed"); return false; }
  BLECharacteristic* rx=service->createCharacteristic(
      cfg::kBleRxUuid,BLECharacteristic::PROPERTY_WRITE |
                      BLECharacteristic::PROPERTY_WRITE_NR);
  tx_characteristic_=service->createCharacteristic(
      cfg::kBleTxUuid,BLECharacteristic::PROPERTY_NOTIFY);
  if (!rx || !tx_characteristic_) {
    setError("BLE characteristic failed"); return false;
  }
  rx->setCallbacks(new M5CanBleRxCallbacks(*this));
  tx_characteristic_->addDescriptor(new BLE2902());
  service->start();
  advertising_=BLEDevice::getAdvertising();
#endif
  if (!advertising_) { setError("BLE advertising unavailable"); return false; }
  advertising_->addServiceUUID(cfg::kBleServiceUuid);
  advertising_->setScanResponse(true);
  advertising_->start();
  setError("OK");
  return true;
}

void BleElmTransport::onConnect() {
  portENTER_CRITICAL(&mux_);
  connected_ = true;
  disconnect_event_ = false;
  rx_head_ = 0;
  rx_tail_ = 0;
  portEXIT_CRITICAL(&mux_);
}

void BleElmTransport::restartAdvertising() {
  if (advertising_) advertising_->start();
}

void BleElmTransport::onDisconnect() {
  portENTER_CRITICAL(&mux_);
  connected_ = false;
  disconnect_event_ = true;
  rx_head_ = 0;
  rx_tail_ = 0;
  portEXIT_CRITICAL(&mux_);
  restartAdvertising();
}

void BleElmTransport::onWrite(const uint8_t* data, size_t length) {
  if (!data || !length) return;

  portENTER_CRITICAL(&mux_);
  for (size_t i = 0; i < length; ++i) {
    const size_t next = (rx_head_ + 1) % kRxBufferSize;
    if (next == rx_tail_) {
      ++stats_.rx_overflow;
      break;
    }
    rx_buffer_[rx_head_] = data[i];
    rx_head_ = next;
    ++stats_.rx_bytes;
  }
  portEXIT_CRITICAL(&mux_);
}

int BleElmTransport::available() const {
  portENTER_CRITICAL(&mux_);
  const size_t count =
      rx_head_ >= rx_tail_
          ? rx_head_ - rx_tail_
          : kRxBufferSize - rx_tail_ + rx_head_;
  portEXIT_CRITICAL(&mux_);
  return static_cast<int>(count);
}

int BleElmTransport::read() {
  portENTER_CRITICAL(&mux_);
  if (rx_head_ == rx_tail_) {
    portEXIT_CRITICAL(&mux_);
    return -1;
  }
  const uint8_t value = rx_buffer_[rx_tail_];
  rx_tail_ = (rx_tail_ + 1) % kRxBufferSize;
  portEXIT_CRITICAL(&mux_);
  return value;
}

size_t BleElmTransport::write(const uint8_t* data, size_t length) {
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  // The legacy notify() broadcasts to every connected client. Never allow
  // it in two-client mode; all responses must use writeTo(conn_id,...).
  (void)data;
  (void)length;
  return 0;
#else
  if (!data || !length || !connected_ || !tx_characteristic_) return 0;

  // 20 bytes is valid before any larger ATT MTU negotiation.
  constexpr size_t kNotificationChunk = 20;
  size_t sent = 0;
  while (sent < length && connected_) {
    const size_t chunk = std::min(kNotificationChunk, length - sent);
    tx_characteristic_->setValue(
        const_cast<uint8_t*>(data + sent), chunk);
    tx_characteristic_->notify();
    sent += chunk;
    if (sent < length) delay(1);
  }

  portENTER_CRITICAL(&mux_);
  stats_.tx_bytes += sent;
  portEXIT_CRITICAL(&mux_);
  return sent;
#endif
}

bool BleElmTransport::takeDisconnectEvent() {
  portENTER_CRITICAL(&mux_);
  const bool event = disconnect_event_;
  disconnect_event_ = false;
  portEXIT_CRITICAL(&mux_);
  return event;
}

BleStats BleElmTransport::stats() const {
  portENTER_CRITICAL(&mux_);
  const BleStats copy = stats_;
  portEXIT_CRITICAL(&mux_);
  return copy;
}

#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
void BleElmTransport::pushEvent(BleClientEvent::Type type,
                               uint16_t id, uint32_t generation,
                               uint8_t byte) {
  const size_t next = (event_head_ + 1) % kEventBufferSize;
  if (next == event_tail_) {
    ++stats_.rx_overflow;
    return;
  }
  BleClientEvent& e = events_[event_head_];
  e.type = type;
  e.connection_id = id;
  e.generation = generation;
  e.byte = byte;
  event_head_ = next;
}

void BleElmTransport::onPeerConnect(uint16_t id) {
  bool accepted = false;
  portENTER_CRITICAL(&mux_);
  for (auto& peer : clients_) {
    if (!peer.active) {
      peer.connection_id = id;
      peer.generation = ++next_generation_;
      peer.mtu = 23;
      peer.active = true;
      connected_ = true;
      pushEvent(BleClientEvent::Type::Connect,id,peer.generation,0);
      accepted = true;
      break;
    }
  }
  if (!accepted) ++stats_.session_rejections;
  portEXIT_CRITICAL(&mux_);
  if (!accepted && server_) {
    server_->disconnect(id);
  } else if (accepted && connectedClientCount() < kMaxClients) {
    // Arduino-ESP32 stops advertising when a peer connects. Continue
    // advertising while there is room for the second authorized peer.
    restartAdvertising();
  }
}

void BleElmTransport::onPeerDisconnect(uint16_t id) {
  portENTER_CRITICAL(&mux_);
  for (auto& peer : clients_) {
    if (peer.active && peer.connection_id == id) {
      pushEvent(BleClientEvent::Type::Disconnect,id,peer.generation,0);
      peer.active = false;
    }
  }
  connected_ = false;
  for (const auto& peer : clients_) if (peer.active) connected_ = true;
  portEXIT_CRITICAL(&mux_);
  restartAdvertising();
}

void BleElmTransport::onPeerMtu(uint16_t id, uint16_t mtu) {
  portENTER_CRITICAL(&mux_);
  for (auto& peer : clients_) if (peer.active && peer.connection_id == id)
    peer.mtu = mtu < 23 ? 23 : mtu;
  portEXIT_CRITICAL(&mux_);
}

void BleElmTransport::onPeerWrite(uint16_t id,
                                  const uint8_t* data, size_t length) {
  if (!data || !length) return;
  portENTER_CRITICAL(&mux_);
  for (const auto& peer : clients_) {
    if (peer.active && peer.connection_id == id) {
      for (size_t i = 0; i < length; ++i) {
        pushEvent(BleClientEvent::Type::Byte,id,peer.generation,data[i]);
        ++stats_.rx_bytes;
      }
      break;
    }
  }
  portEXIT_CRITICAL(&mux_);
}

bool BleElmTransport::pollClientEvent(BleClientEvent& event) {
  portENTER_CRITICAL(&mux_);
  if (event_head_ == event_tail_) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }
  event = events_[event_tail_];
  event_tail_ = (event_tail_ + 1) % kEventBufferSize;
  portEXIT_CRITICAL(&mux_);
  return true;
}

bool BleElmTransport::isClientConnected(uint16_t id, uint32_t gen) const {
  portENTER_CRITICAL(&mux_);
  bool found = false;
  for (const auto& peer : clients_)
    if (peer.active && peer.connection_id == id && peer.generation == gen)
      found = true;
  portEXIT_CRITICAL(&mux_);
  return found;
}

uint8_t BleElmTransport::connectedClientCount() const {
  portENTER_CRITICAL(&mux_);
  uint8_t count = 0;
  for (const auto& peer : clients_) if (peer.active) ++count;
  portEXIT_CRITICAL(&mux_);
  return count;
}

size_t BleElmTransport::writeTo(uint16_t id, uint32_t gen,
                                const uint8_t* data, size_t length) {
  if (!server_ || !tx_characteristic_ || !data || !length) return 0;
  uint16_t mtu = 23;
  bool active = false;
  portENTER_CRITICAL(&mux_);
  for (const auto& peer : clients_) {
    if (peer.active && peer.connection_id == id && peer.generation == gen) {
      active = true;
      mtu = peer.mtu;
      break;
    }
  }
  portEXIT_CRITICAL(&mux_);
  if (!active) return 0;
  // The established meter bench accepts 20-byte notifications. A larger
  // negotiated MTU does not imply that its application RX queue accepts
  // larger chunks; keep the dual stability baseline compatible.
  const size_t max_payload = std::min<size_t>(20, mtu - 3);
  size_t sent = 0;
  uint32_t chunks = 0;
  while (sent < length && isClientConnected(id, gen)) {
    const uint16_t chunk = static_cast<uint16_t>(
        std::min(max_payload,length - sent));
    // This API targets precisely one GATT connection; normal
    // BLECharacteristic::notify() broadcasts and must never be used here.
    // os_mbuf ownership transfers to NimBLE whether or not the controller
    // ultimately transmits the notification. The first argument is the
    // destination connection handle, never a broadcast list.
    os_mbuf* packet = ble_hs_mbuf_from_flat(data + sent, chunk);
    if (!packet || ble_gattc_notify_custom(
            id,tx_characteristic_->getHandle(),packet) != 0) break;
    sent += chunk;
    ++chunks;
    if (sent < length) delay(1);
  }
  portENTER_CRITICAL(&mux_);
  stats_.tx_bytes += sent;
  stats_.targeted_notifications += chunks;
  portEXIT_CRITICAL(&mux_);
  return sent;
}
#endif

}  // namespace m5can
