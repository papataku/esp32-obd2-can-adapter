#include "ble_elm_transport.h"

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "app_config.h"

namespace m5can {

class M5CanBleServerCallbacks final : public BLEServerCallbacks {
 public:
  explicit M5CanBleServerCallbacks(BleElmTransport& owner) : owner_(owner) {}

  void onConnect(BLEServer*) override {
    owner_.onConnect();
  }

  void onDisconnect(BLEServer*) override {
    owner_.onDisconnect();
  }

 private:
  BleElmTransport& owner_;
};

class M5CanBleRxCallbacks final : public BLECharacteristicCallbacks {
 public:
  explicit M5CanBleRxCallbacks(BleElmTransport& owner) : owner_(owner) {}

  void onWrite(BLECharacteristic* characteristic) override {
    const std::string value = characteristic->getValue();
    if (!value.empty()) {
      owner_.onWrite(
          reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
  }

 private:
  BleElmTransport& owner_;
};

void BleElmTransport::setError(const char* message) {
  std::strncpy(last_error_, message ? message : "unknown",
               sizeof(last_error_) - 1);
  last_error_[sizeof(last_error_) - 1] = '\0';
}

bool BleElmTransport::begin() {
  BLEDevice::init(cfg::kBleDeviceName);

  BLEServer* server = BLEDevice::createServer();
  if (!server) {
    setError("BLE server create failed");
    return false;
  }
  server->setCallbacks(new M5CanBleServerCallbacks(*this));

  BLEService* service = server->createService(cfg::kBleServiceUuid);
  if (!service) {
    setError("BLE service create failed");
    return false;
  }

  BLECharacteristic* rx = service->createCharacteristic(
      cfg::kBleRxUuid,
      BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_WRITE_NR);
  tx_characteristic_ = service->createCharacteristic(
      cfg::kBleTxUuid,
      BLECharacteristic::PROPERTY_NOTIFY);
  if (!rx || !tx_characteristic_) {
    setError("BLE characteristic create failed");
    return false;
  }

  rx->setCallbacks(new M5CanBleRxCallbacks(*this));
  tx_characteristic_->addDescriptor(new BLE2902());

  service->start();

  advertising_ = BLEDevice::getAdvertising();
  if (!advertising_) {
    setError("BLE advertising unavailable");
    return false;
  }
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

}  // namespace m5can
