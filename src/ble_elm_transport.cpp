#include "ble_elm_transport.h"
#include "diagnostic_error_catalog.h"
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
#include <NimBLEDevice.h>
#if defined(ARDUINO_ARCH_ESP32)
#include "nimble/nimble/host/src/ble_hs_conn_priv.h"
extern "C" void ble_hs_lock(void);
extern "C" void ble_hs_unlock(void);
#endif
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
#include "ble_notify_chunk.h"
#if defined(M5CAN_LOG_STREAM_V2_EXPERIMENTAL)
#include "m5can_log_stream_v2_gatt.h"
#include "m5can_log_v2_peer_policy.h"
#endif

namespace m5can {
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
class M5CanBleServerCallbacks final : public NimBLEServerCallbacks {
 public:
  explicit M5CanBleServerCallbacks(BleElmTransport& owner) : owner_(owner) {
    gap_owner_ = &owner;
  }
  static int gapEvent(ble_gap_event* event, void*) {
    if (gap_owner_ && event && event->type == BLE_GAP_EVENT_DISCONNECT) {
      const auto& d = event->disconnect.conn;
      gap_owner_->recordGapDisconnect(d.conn_handle, event->disconnect.reason,
          d.conn_itvl, d.conn_latency, d.supervision_timeout);
    }
    return 0; // observer only: never consume or alter GAP/session state
  }
  void onConnect(NimBLEServer*, ble_gap_conn_desc* d) override {
    if (d) owner_.onPeerConnect(d->conn_handle,
        static_cast<uint16_t>((static_cast<uint16_t>(d->peer_ota_addr.val[1]) << 8) |
                               d->peer_ota_addr.val[0]));
  }
  void onDisconnect(NimBLEServer*, ble_gap_conn_desc* d) override {
    if (d) owner_.onPeerDisconnect(d->conn_handle);
  }
  void onMTUChange(uint16_t mtu, ble_gap_conn_desc* d) override {
    if (d) owner_.onPeerMtu(d->conn_handle, mtu);
  }
 private:
  static BleElmTransport* gap_owner_;
  BleElmTransport& owner_;
};
BleElmTransport* M5CanBleServerCallbacks::gap_owner_=nullptr;
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
#if defined(M5CAN_LOG_STREAM_V2_EXPERIMENTAL)
class M5CanLogControlCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  explicit M5CanLogControlCallbacks(BleElmTransport& owner):owner_(owner){}
  void onWrite(NimBLECharacteristic* characteristic,
               ble_gap_conn_desc* desc) override {
    if(!characteristic || !desc)return;
    // A Just Works encrypted but unbonded connection must not export logs.
    // Bonding alone is not sufficient for production authorization; no
    // runtime V2 start is enabled until explicit allow-listing is added.
    bool secured=desc->sec_state.encrypted && desc->sec_state.bonded;
    // Just Works bonding has no MITM proof; never make a retained RAW
    // vehicle log readable by *any* arbitrary newly bonded BLE peer.
    // Operator must explicitly provision the resolved 48-bit identity
    // into a NON-DEFAULT evaluation profile before log transfer works.
#if defined(M5CAN_LOG_V2_ALLOWED_PEER_ID48)
    uint64_t identity=0;
    for(unsigned i=0;i<6;++i)
      identity |= static_cast<uint64_t>(desc->peer_id_addr.val[i])<<(i*8);
    secured=secured && identity==
        (static_cast<uint64_t>(M5CAN_LOG_V2_ALLOWED_PEER_ID48)&0xFFFFFFFFFFFFULL);
#else
    secured=false; // fail closed until a user-authorized peer is enrolled
#endif
    const std::string value=characteristic->getValue();
    if(value.size()>0)
      owner_.onLogWrite(desc->conn_handle,
          reinterpret_cast<const uint8_t*>(value.data()),value.size(),secured);
  }
 private:
  BleElmTransport& owner_;
};
class M5CanLogDataCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  explicit M5CanLogDataCallbacks(BleElmTransport& owner):owner_(owner){}
  void onSubscribe(NimBLECharacteristic*,ble_gap_conn_desc* desc,
                   uint16_t subscription) override {
    if(desc)owner_.onLogSubscribe(desc->conn_handle,(subscription&1U)!=0);
  }
 private:
  BleElmTransport& owner_;
};
#endif

#else
class M5CanBleServerCallbacks final : public BLEServerCallbacks {
 public:
  explicit M5CanBleServerCallbacks(BleElmTransport& owner) : owner_(owner) {}
  void onConnect(BLEServer*) override { owner_.onConnect(); }
  void onDisconnect(BLEServer*) override { owner_.onDisconnect(); }
  void onMtuChanged(BLEServer*,esp_ble_gatts_cb_param_t* p) override {
    if(p)owner_.onLegacyMtu(p->mtu.mtu);
  }
#if defined(ESP_PLATFORM)
  // The Arduino-ESP32 Bluedroid callbacks expose the controller's true
  // disconnect reason and per-connection ID in the two-argument overload.
  // Keep the one-argument overload for the existing transport bookkeeping.
  void onConnect(BLEServer*, esp_ble_gatts_cb_param_t* event) override {
    if (event) owner_.onConnectDetails(event->connect.conn_id);
  }
  void onDisconnect(BLEServer*, esp_ble_gatts_cb_param_t* event) override {
    if (event) {
      owner_.onDisconnectDetails(
          event->disconnect.conn_id,
          static_cast<uint16_t>(event->disconnect.reason));
    }
  }
#endif
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
#if defined(ARDUINO_ARCH_ESP32)
  // NimBLE-Arduino 1.4 uses CONFIG_BT_NIMBLE_PINNED_TO_CORE, not the
  // MYNEWT_VAL name used by other NimBLE ports. Reject silent core0 fallback.
  static_assert(CONFIG_BT_NIMBLE_PINNED_TO_CORE == M5CAN_DUAL_BLE_APP_CORE,
                "NimBLE host and BLE worker must share the configured core");
#endif
  NimBLEDevice::init(cfg::kBleDeviceName);
  NimBLEDevice::setMTU(247);
  NimBLEServer* server=NimBLEDevice::createServer();
  if (!server) { setError("NimBLE server failed"); return false; }
  server_=server;
  server->setCallbacks(new M5CanBleServerCallbacks(*this));
  NimBLEDevice::setCustomGapHandler(M5CanBleServerCallbacks::gapEvent);
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
#if defined(M5CAN_LOG_STREAM_V2_EXPERIMENTAL)
  // A binary log path on independent UUIDs: neither the ASCII ELM parser
  // nor its write/notify queue ever sees V2 control or 244-byte DATA.
  // Enforce encrypted GATT writes and fail closed for unbonded clients.
  NimBLEDevice::setSecurityAuth(true,false,true);
  NimBLEService* log=server->createService(
      m5can_log_stream_v2::kGattService);
  if(!log) {setError("V2 log service failed");return false;}
  NimBLECharacteristic* log_ctrl=log->createCharacteristic(
      m5can_log_stream_v2::kGattControl,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
  log_tx_characteristic_=log->createCharacteristic(
      m5can_log_stream_v2::kGattData,NIMBLE_PROPERTY::NOTIFY);
  if(!log_ctrl || !log_tx_characteristic_) {
    setError("V2 log characteristics failed");return false;
  }
  log_ctrl->setCallbacks(new M5CanLogControlCallbacks(*this));
  log_tx_characteristic_->setCallbacks(new M5CanLogDataCallbacks(*this));
  log->start();
#endif
  advertising_=NimBLEDevice::getAdvertising();
#else
  BLEDevice::init(cfg::kBleDeviceName);
#if defined(M5CAN_BLE_MTU_EXPERIMENTAL)
  BLEDevice::setMTU(247);
#endif
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
  ++stats_.connect_count;stats_.last_connect_ms=millis();
  connected_ = true;
  peer_mtu_=stats_.peer_mtu=23;
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
  ++stats_.disconnect_count;stats_.last_disconnect_ms=millis();
  stats_.last_disconnect_reason_known=false;
  connected_ = false;
  peer_mtu_=stats_.peer_mtu=23;
  disconnect_event_ = true;
  rx_head_ = 0;
  rx_tail_ = 0;
  portEXIT_CRITICAL(&mux_);
  restartAdvertising();
}

void BleElmTransport::onConnectDetails(uint16_t conn_id) {
  portENTER_CRITICAL(&mux_);
  stats_.last_connect_conn_id = conn_id;
  portEXIT_CRITICAL(&mux_);
}

void BleElmTransport::onDisconnectDetails(uint16_t conn_id, uint16_t reason) {
  portENTER_CRITICAL(&mux_);
  stats_.last_disconnect_conn_id = conn_id;
  stats_.last_disconnect_reason = reason;
  stats_.last_disconnect_reason_known = true;
  portEXIT_CRITICAL(&mux_);
}

void BleElmTransport::onLegacyMtu(uint16_t mtu) {
  portENTER_CRITICAL(&mux_);
  peer_mtu_=stats_.peer_mtu=mtu>=23?mtu:23;
  portEXIT_CRITICAL(&mux_);
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
  // it in two-client mode; all responses must use queueTo(conn_id,...).
  (void)data;
  (void)length;
  return 0;
#else
  if (!data || !length || !connected_ || !tx_characteristic_) return 0;

  // 20 bytes is valid before any larger ATT MTU negotiation.
  constexpr size_t kNotificationChunk = 20;
  size_t notification_chunk=kNotificationChunk;
#if defined(M5CAN_BLE_MTU_EXPERIMENTAL)
  portENTER_CRITICAL(&mux_);notification_chunk=ble_notify_chunk_size(peer_mtu_);portEXIT_CRITICAL(&mux_);
#endif
  size_t sent = 0;
  while (sent < length && connected_) {
    const size_t chunk = std::min(notification_chunk, length - sent);
    tx_characteristic_->setValue(
        const_cast<uint8_t*>(data + sent), chunk);
    tx_characteristic_->notify();
    portENTER_CRITICAL(&mux_);
    ++stats_.tx_notifications;
    stats_.max_notify_chunk=std::max<size_t>(stats_.max_notify_chunk,chunk);
    portEXIT_CRITICAL(&mux_);
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
    pushDiagnostic(BleDiagnosticEvent::Kind::Error,id,0,false,0,0,0,
                   static_cast<uint16_t>(DiagnosticError::BleRxQueueOverflow),
                   static_cast<uint32_t>(stats_.rx_overflow));
    return;
  }
  BleClientEvent& e = events_[event_head_];
  e.type = type;
  e.connection_id = id;
  e.generation = generation;
  e.byte = byte;
  event_head_ = next;
}

void BleElmTransport::onPeerConnect(uint16_t id, uint16_t address_suffix) {
  bool accepted = false;
  portENTER_CRITICAL(&mux_);
  for (auto& peer : clients_) {
    if (!peer.active) {
      peer.connection_id = id;
      peer.generation = ++next_generation_;
      peer.mtu = 23;
      peer.active = true;
      peer.gap_disconnect_seen = false;
      peer.pending_head=peer.pending_size=0;
      peer.progress_ms=millis();peer.stalled_reported=false;peer.tx_aborted=false;
#if defined(M5CAN_LOG_STREAM_V2_EXPERIMENTAL)
      peer.log_subscribed=false;peer.log_secured=false;
#endif
      peer.traffic = {};
      peer.traffic.connected = true;
      peer.traffic.connection_id = id;
      peer.traffic.generation = peer.generation;
      peer.traffic.address_suffix = address_suffix;
      connected_ = true;
      ++stats_.connect_count;stats_.last_connect_ms=millis();stats_.last_connect_conn_id=id;
      pushEvent(BleClientEvent::Type::Connect,id,peer.generation,0);
      pushDiagnostic(BleDiagnosticEvent::Kind::Connect,id);
      accepted = true;
      break;
    }
  }
  if (!accepted) {
    ++stats_.session_rejections;
    pushDiagnostic(BleDiagnosticEvent::Kind::Reject,id,0,false,0,0,0,
                   static_cast<uint16_t>(DiagnosticError::BleNoClientSlot),0);
  }
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
      // GAP observer isn't guaranteed for every NimBLE/server exit path.
      // Preserve an explicit unknown-reason event rather than silently
      // treating a server callback as a clean or explained disconnect.
      ++stats_.disconnect_count;stats_.last_disconnect_ms=millis();stats_.last_disconnect_conn_id=id;
      if(!peer.gap_disconnect_seen)stats_.last_disconnect_reason_known=false;
      if(!peer.gap_disconnect_seen)
        pushDiagnostic(BleDiagnosticEvent::Kind::Disconnect,id,0,false);
      pushEvent(BleClientEvent::Type::Disconnect,id,peer.generation,0);
      peer.gap_disconnect_seen=false;
      peer.active = false;
      peer.pending_head=peer.pending_size=0;
#if defined(M5CAN_LOG_STREAM_V2_EXPERIMENTAL)
      peer.log_subscribed=false;peer.log_secured=false;
#endif
      peer.traffic.connected = false;
    }
  }
  connected_ = false;
  for (const auto& peer : clients_) if (peer.active) connected_ = true;
  portEXIT_CRITICAL(&mux_);
  restartAdvertising();
}

// Caller holds mux_: callbacks enqueue bounded POD only, main loop prints.
void BleElmTransport::pushDiagnostic(BleDiagnosticEvent::Kind kind, uint16_t id,
    int reason, bool known, uint16_t interval, uint16_t latency,
    uint16_t supervision,uint16_t code,uint32_t detail) {
  const uint32_t seq = ++diagnostic_seq_;
  const size_t next = (diagnostic_head_ + 1) % kDiagnosticSlots;
  if (next == diagnostic_tail_) {
    ++diagnostic_drops_;
    // Preserve the newest hard-link evidence under event storms. A ring
    // filled with routine errors must not hide the actual GAP disconnect.
    // Displaced records are explicitly counted as lost. Bounded O(1),
    // still no allocations, USB, or NVS in NimBLE's callback context.
    const bool critical=kind==BleDiagnosticEvent::Kind::Disconnect ||
        kind==BleDiagnosticEvent::Kind::Reject ||
        (kind==BleDiagnosticEvent::Kind::Error &&
         (code==static_cast<uint16_t>(DiagnosticError::BleNotifySendFailed) ||
          code==static_cast<uint16_t>(DiagnosticError::CanBusOff) ||
          code==static_cast<uint16_t>(DiagnosticError::CanDriverError)));
    if(!critical)return;
    diagnostic_tail_=(diagnostic_tail_+1)%kDiagnosticSlots;
  }
  auto& e = diagnostics_[diagnostic_head_];
  e = BleDiagnosticEvent{};
  e.kind=kind; e.sequence=seq; e.uptime_ms=millis();
  e.connection_id=id; e.reason=reason; e.reason_known=known;
  e.error_code=code; e.error_detail=detail;
  e.interval=interval; e.latency=latency; e.supervision=supervision;
  for (const auto& p : clients_) if (p.connection_id==id && p.generation) {
    e.generation=p.generation; e.address_suffix=p.traffic.address_suffix;
    e.mtu=p.mtu; e.rx_bytes=p.traffic.rx_bytes; e.tx_bytes=p.traffic.tx_bytes;
    e.retries=p.traffic.notify_retries; e.failed=p.traffic.notify_failures;
    break;
  }
  diagnostic_head_=next;
}
void BleElmTransport::recordGapDisconnect(uint16_t id, int reason,
    uint16_t interval, uint16_t latency, uint16_t supervision) {
  portENTER_CRITICAL(&mux_);
  stats_.last_disconnect_conn_id=id;
  stats_.last_disconnect_reason=reason;
  stats_.last_disconnect_reason_known=true;
  for(auto& peer:clients_)
    if(peer.active && peer.connection_id==id)
      peer.gap_disconnect_seen=true;
  pushDiagnostic(BleDiagnosticEvent::Kind::Disconnect,id,reason,true,
                 interval,latency,supervision);
  portEXIT_CRITICAL(&mux_);
}
void BleElmTransport::recordDiagnosticError(
    uint16_t id,uint32_t generation,uint16_t code,uint32_t detail) {
  portENTER_CRITICAL(&mux_);
  bool found=false;
  for(const auto& peer:clients_) {
    if(peer.active && peer.connection_id==id &&
       peer.generation==generation) {found=true;break;}
  }
  const size_t previous_head=diagnostic_head_;
  pushDiagnostic(BleDiagnosticEvent::Kind::Error,id,0,false,0,0,0,
                 code,detail);
  if(!found && diagnostic_head_!=previous_head) {
    // An old in-flight transaction may finish after the peer disconnected
    // or its numerical handle was reused. Preserve the OLD generation and
    // never invent the current peer's identity/traffic for that error.
    auto& logged=diagnostics_[previous_head];
    logged.generation=generation;
    logged.address_suffix=0;
    logged.mtu=0;
    logged.rx_bytes=0;logged.tx_bytes=0;
    logged.retries=0;logged.failed=0;
  }
  portEXIT_CRITICAL(&mux_);
}
bool BleElmTransport::pollDiagnostic(BleDiagnosticEvent& event) {
  portENTER_CRITICAL(&mux_);
  if (diagnostic_head_==diagnostic_tail_) {
    portEXIT_CRITICAL(&mux_); return false;
  }
  event=diagnostics_[diagnostic_tail_];
  diagnostic_tail_=(diagnostic_tail_+1)%kDiagnosticSlots;
  portEXIT_CRITICAL(&mux_); return true;
}
uint32_t BleElmTransport::diagnosticDrops() const {
  portENTER_CRITICAL(&mux_); const auto drops=diagnostic_drops_;
  portEXIT_CRITICAL(&mux_); return drops;
}

void BleElmTransport::onPeerMtu(uint16_t id, uint16_t mtu) {
  portENTER_CRITICAL(&mux_);
  for (auto& peer : clients_) if (peer.active && peer.connection_id == id)
    { peer.mtu = mtu < 23 ? 23 : mtu; peer.traffic.mtu = peer.mtu; }
  portEXIT_CRITICAL(&mux_);
}

void BleElmTransport::onPeerWrite(uint16_t id,
                                  const uint8_t* data, size_t length) {
  if (!data || !length) return;
  portENTER_CRITICAL(&mux_);
  for (auto& peer : clients_) {
    if (peer.active && peer.connection_id == id) {
      ++peer.traffic.rx_writes;
      for (size_t i = 0; i < length; ++i) {
        const size_t before_overflow=stats_.rx_overflow;
        pushEvent(BleClientEvent::Type::Byte,id,peer.generation,data[i]);
        if (stats_.rx_overflow != before_overflow) break;
        ++stats_.rx_bytes;
        ++peer.traffic.rx_bytes;
      }
      peer.traffic.last_rx_ms=millis();
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

#if defined(M5CAN_LOG_STREAM_V2_EXPERIMENTAL)
void BleElmTransport::onLogSubscribe(uint16_t id,bool subscribed) {
  portENTER_CRITICAL(&mux_);
  for(auto& peer:clients_) {
    if(peer.active && peer.connection_id==id) {
      peer.log_subscribed=subscribed;
      if(!subscribed)peer.log_secured=false;
    }
  }
  portEXIT_CRITICAL(&mux_);
}
void BleElmTransport::onLogWrite(uint16_t id,const uint8_t* data,
                                 size_t count,bool secured) {
  if(!secured || !data || (count!=20 && count!=28))return;
  m5can_log_stream_v2::Control parsed{};
  if(!m5can_log_stream_v2::decodeControl(data,count,parsed))return;
  portENTER_CRITICAL(&mux_);
  for(auto& peer:clients_) {
    if(!peer.active || peer.connection_id!=id)continue;
    const uint8_t next=(log_control_head_+1)%kLogControlSlots;
    if(next==log_control_tail_) {
      ++stats_.rx_overflow;
      break; // bounded all-or-nothing, no partial frame leaks to ELM
    }
    LogControlMessage& entry=log_control_[log_control_head_];
    entry.peer=id;entry.generation=peer.generation;
    entry.count=static_cast<uint8_t>(count);
    std::memcpy(entry.data,data,count);
    log_control_head_=next;
    peer.log_secured=true;
    break;
  }
  portEXIT_CRITICAL(&mux_);
}
bool BleElmTransport::pollLogControl(LogControlMessage& message) {
  portENTER_CRITICAL(&mux_);
  if(log_control_head_==log_control_tail_) {
    portEXIT_CRITICAL(&mux_);return false;
  }
  message=log_control_[log_control_tail_];
  log_control_tail_=(log_control_tail_+1)%kLogControlSlots;
  portEXIT_CRITICAL(&mux_);
  return isClientConnected(message.peer,message.generation);
}
bool BleElmTransport::logPeerAuthorized(uint16_t id,uint32_t gen) const {
  bool authorized=false;
  portENTER_CRITICAL(&mux_);
  for(const auto& peer:clients_) {
    if(peer.connection_id==id && peer.generation==gen &&
       m5can_log_stream_v2::PeerQosPolicy::eligible(
           peer.active,peer.log_subscribed,peer.log_secured,
           peer.mtu>=247,peer.tx_aborted))
      authorized=true;
  }
  portEXIT_CRITICAL(&mux_);
  if(!authorized)return false;
  // A previously authenticated writer is not enough: recheck the actual
  // live encrypted/bonded link and the exact generation at every poll.
  ble_gap_conn_desc desc{};
  return ble_gap_conn_find(id,&desc)==0 &&
         desc.sec_state.encrypted && desc.sec_state.bonded;
}
bool BleElmTransport::logPeerReady(uint16_t id,uint32_t gen) const {
  if(!logPeerAuthorized(id,gen))return false;
  // ASCII ELM replies/CAN diagnostics take precedence over bulk V2 logs.
  // This is TRANSIENT backpressure, not lost subscriber authorization.
  bool eligible=false;
  bool all_elm_queues_idle=true;
  portENTER_CRITICAL(&mux_);
  for(const auto& peer:clients_) {
    // Both SmartRing and iPad/JC share NimBLE's packet budget. Do not
    // enqueue a 244B V2 frame while EITHER active peer still has an
    // ordinary CAN/PID response waiting in the application queue.
    // Inactive slots are not a reason to stall the log sender.
    all_elm_queues_idle &=
        m5can_log_stream_v2::PeerQosPolicy::elmQueueIdle(
            peer.active,peer.pending_size);
    if(peer.connection_id==id && peer.generation==gen)
      eligible=m5can_log_stream_v2::PeerQosPolicy::eligible(
          peer.active,peer.log_subscribed,peer.log_secured,
          peer.mtu>=247,peer.tx_aborted);
  }
  portEXIT_CRITICAL(&mux_);
  return m5can_log_stream_v2::PeerQosPolicy::maySend(
      eligible,all_elm_queues_idle);
}
#endif

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

BlePeersSnapshot BleElmTransport::peerTraffic() const {
  BlePeersSnapshot snapshot{};
  portENTER_CRITICAL(&mux_);
  for (size_t i=0;i<kMaxClients;++i) snapshot.peers[i]=clients_[i].traffic;
  portEXIT_CRITICAL(&mux_);
  return snapshot;
}

#ifndef M5CAN_DUAL_NOTIFY_CHUNK_BYTES
#define M5CAN_DUAL_NOTIFY_CHUNK_BYTES 20
#endif
static_assert(M5CAN_DUAL_NOTIFY_CHUNK_BYTES >= 20 &&
              M5CAN_DUAL_NOTIFY_CHUNK_BYTES <= 244,
              "dual notification chunk must fit ATT MTU 247");
// NimBLE returns success when it merely appends to bhc_tx_q. Without a
// producer bound, a slow peer can consume the shared mbuf pool despite every
// previous notify returning zero. Read its real controller/host backlog under
// the host lock; do not hold the application spinlock across NimBLE calls.
static bool notifyCredit(uint16_t id,uint16_t& pending,uint16_t& queued) {
  bool present=false;
#if defined(ARDUINO_ARCH_ESP32)
  ble_hs_lock();
  const auto* conn=ble_hs_conn_find(id);
  if(conn) {
    present=true;
    pending=conn->bhc_outstanding_pkts;
    const os_mbuf_pkthdr* entry;
    STAILQ_FOREACH(entry,&conn->bhc_tx_q,omp_next) {
      if(queued>=64)break;
      ++queued;
    }
    pending+=queued;
  }
  ble_hs_unlock();
#else
  present=test_notify_snapshot(id,pending,queued);
#endif
  // Keep a small pipeline preloaded for controller-completion processing.
  // An empty-only host queue loses throughput because the application task
  // cannot refill it within a radio event. Bound both peer ownership and
  // shared free capacity instead of removing all host staging.
  return present && queued<3 && pending<8;
}

#if defined(M5CAN_LOG_STREAM_V2_EXPERIMENTAL)
bool BleElmTransport::notifyLogFrame(uint16_t id,uint32_t gen,
                                    const uint8_t* frame,size_t n) {
  // The dedicated GATT service MUST NOT use the legacy byte-stream queue:
  // splitting or interleaving a 244-byte binary packet corrupts framing.
  if(!log_tx_characteristic_ || !frame || n<28 || n>244 ||
     !logPeerReady(id,gen))return false;
  m5can_log_stream_v2::Frame decoded{};
  if(!m5can_log_stream_v2::decode(frame,n,decoded) ||
     decoded.kind==m5can_log_stream_v2::Kind::Ack)return false;
  uint16_t host_pending=0,host_queued=0;
  if(os_msys_num_free()<8 ||
     !notifyCredit(id,host_pending,host_queued))return false;
  os_mbuf* packet=ble_hs_mbuf_from_flat(frame,n);
  if(!packet)return false;
  const int rc=ble_gattc_notify_custom(
      id,log_tx_characteristic_->getHandle(),packet);
  if(rc!=0)return false;
  portENTER_CRITICAL(&mux_);
  for(auto& peer:clients_) {
    if(peer.active && peer.connection_id==id && peer.generation==gen) {
      peer.traffic.tx_bytes+=n;
      ++peer.traffic.notifications;
      peer.traffic.last_tx_ms=millis();
      stats_.tx_bytes+=n;
      ++stats_.targeted_notifications;
    }
  }
  portEXIT_CRITICAL(&mux_);
  return true;
}
#endif

bool BleElmTransport::responseReady(uint16_t id,uint32_t gen) const {
#if defined(M5CAN_SYNC_NOTIFY_DIAGNOSTIC)
  return isClientConnected(id,gen);
#else
  bool ready=false;
  portENTER_CRITICAL(&mux_);
  for(const auto& peer:clients_)
    if(peer.active && !peer.tx_aborted && peer.connection_id==id && peer.generation==gen)
      // Leave room for one maximum 48-frame RAW result plus session output.
      ready=peer.pending_size<=1024;
  portEXIT_CRITICAL(&mux_);
  return ready;
#endif
}

size_t BleElmTransport::queueTo(uint16_t id,uint32_t gen,
                                const uint8_t* data,size_t length) {
#if defined(M5CAN_SYNC_NOTIFY_DIAGNOSTIC)
  return writeTo(id,gen,data,length);
#else
  if(!data || !length || !server_ || !tx_characteristic_)return 0;
  bool overflow=false,matched=false;
  size_t accepted=0;
  portENTER_CRITICAL(&mux_);
  for(auto& peer:clients_) {
    if(!peer.active || peer.connection_id!=id || peer.generation!=gen)continue;
    matched=true;
    if(peer.tx_aborted)break;
    if(length>sizeof(peer.pending)-peer.pending_size) {
      ++stats_.tx_queue_overflows;++stats_.notify_failures;
      ++peer.traffic.notify_failures;
      peer.pending_size=0;peer.tx_aborted=true;
      pushDiagnostic(BleDiagnosticEvent::Kind::Error,id,0,false,0,0,0,
          static_cast<uint16_t>(DiagnosticError::BleNotifySendFailed),0x10000U);
      overflow=true;
      break;
    }
    if(!peer.pending_size)peer.progress_ms=millis();
    for(size_t n=0;n<length;++n)
      peer.pending[(peer.pending_head+peer.pending_size+n)%sizeof(peer.pending)]=data[n];
    peer.pending_size+=length;
    stats_.tx_queue_peak=std::max(stats_.tx_queue_peak,peer.pending_size);
    accepted=length;
    break;
  }
  portEXIT_CRITICAL(&mux_);
  // A partial response must never be silently followed by the next prompt.
  if(!matched)recordDiagnosticError(id,gen,static_cast<uint16_t>(
      DiagnosticError::BlePeerGenerationStale),gen);
  if(overflow && isClientConnected(id,gen))server_->disconnect(id);
  return accepted;
#endif
}

void BleElmTransport::pollTx() {
  const uint8_t first=tx_round_robin_;
  tx_round_robin_=(tx_round_robin_+1)%kMaxClients;
  for(unsigned scan=0;scan<kMaxClients;++scan) {
    const unsigned slot=(first+scan)%kMaxClients;
    uint16_t id=0,chunk=0;
    uint32_t gen=0;
    uint8_t data[M5CAN_DUAL_NOTIFY_CHUNK_BYTES]{};
    portENTER_CRITICAL(&mux_);
    auto& peer=clients_[slot];
    if(peer.active && !peer.tx_aborted && peer.pending_size) {
      id=peer.connection_id;gen=peer.generation;
      chunk=std::min<uint16_t>(peer.pending_size,
          std::min<uint16_t>(sizeof(data),peer.mtu-3));
      for(unsigned n=0;n<chunk;++n)data[n]=peer.pending[(peer.pending_head+n)%sizeof(peer.pending)];
    }
    portEXIT_CRITICAL(&mux_);
    if(!chunk)continue;
    uint16_t host_pending=0,host_queued=0;
    const int free_blocks=os_msys_num_free();
    const bool credit=notifyCredit(id,host_pending,host_queued);
    portENTER_CRITICAL(&mux_);
    stats_.msys_min_free=std::min<uint16_t>(stats_.msys_min_free,std::max(0,free_blocks));
    stats_.host_pending_peak=std::max(stats_.host_pending_peak,host_pending);
    stats_.host_queue_peak=std::max(stats_.host_queue_peak,host_queued);
    if(!credit || free_blocks<6)++stats_.credit_waits;
    portEXIT_CRITICAL(&mux_);
    int result=BLE_HS_EAGAIN;
    bool attempted=false,allocated=false;
    if(credit && free_blocks>=6 && isClientConnected(id,gen)) {
      attempted=true;
      os_mbuf* packet=ble_hs_mbuf_from_flat(data,chunk);
      allocated=packet!=nullptr;
      result=packet?ble_gattc_notify_custom(id,tx_characteristic_->getHandle(),packet):BLE_HS_ENOMEM;
    }
    bool disconnect=false;
    portENTER_CRITICAL(&mux_);
    if(peer.active && peer.connection_id==id && peer.generation==gen) {
      if(attempted && result==0) {
        peer.pending_head=(peer.pending_head+chunk)%sizeof(peer.pending);
        peer.pending_size-=chunk;peer.progress_ms=millis();peer.stalled_reported=false;
        peer.traffic.tx_bytes+=chunk;++peer.traffic.notifications;
        peer.traffic.last_tx_ms=millis();stats_.tx_bytes+=chunk;++stats_.targeted_notifications;
      } else {
        if(attempted) {
          ++stats_.notify_retries;++peer.traffic.notify_retries;
          if(!allocated)++stats_.mbuf_alloc_failures;else ++stats_.notify_api_failures;
          stats_.memory_error_ms=millis();stats_.memory_error_conn=id;stats_.memory_error_code=result;
          if(!peer.stalled_reported) {
            pushDiagnostic(BleDiagnosticEvent::Kind::Error,id,0,false,0,0,0,
                static_cast<uint16_t>(DiagnosticError::BleNotifyBackpressure),1);
            peer.stalled_reported=true;
          }
        }
        const bool permanent=attempted && result!=BLE_HS_ENOMEM &&
            result!=BLE_HS_EAGAIN && result!=BLE_HS_EBUSY;
        // Liveness bound is per-peer lack of progress, not time spent sending
        // an entire response. Recovery always resumes the same unsent bytes.
        if(permanent || static_cast<uint32_t>(millis()-peer.progress_ms)>=5000) {
          ++stats_.notify_failures;++peer.traffic.notify_failures;
          if(!permanent)++stats_.tx_stalls;
          pushDiagnostic(BleDiagnosticEvent::Kind::Error,id,0,false,0,0,0,
              static_cast<uint16_t>(DiagnosticError::BleNotifySendFailed),
              permanent?static_cast<uint32_t>(result):0x20000U);
          peer.pending_size=0;peer.tx_aborted=true;disconnect=true;
        }
      }
    }
    portEXIT_CRITICAL(&mux_);
    if(disconnect && isClientConnected(id,gen))server_->disconnect(id);
  }
}

#if defined(M5CAN_SYNC_NOTIFY_DIAGNOSTIC)
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
  if (!active) {
    recordDiagnosticError(id,gen,static_cast<uint16_t>(
        DiagnosticError::BlePeerGenerationStale),gen);
    return 0;
  }
  // The established meter bench accepts 20-byte notifications. A larger
  // negotiated MTU does not imply that its application RX queue accepts
  // larger chunks; keep the dual stability baseline compatible.
#ifndef M5CAN_DUAL_NOTIFY_CHUNK_BYTES
#define M5CAN_DUAL_NOTIFY_CHUNK_BYTES 20
#endif
  static_assert(M5CAN_DUAL_NOTIFY_CHUNK_BYTES >= 20 &&
                M5CAN_DUAL_NOTIFY_CHUNK_BYTES <= 244,
                "dual notification chunk must fit ATT MTU 247");
  const size_t max_payload = std::min<size_t>(
      M5CAN_DUAL_NOTIFY_CHUNK_BYTES, mtu - 3);
  size_t sent = 0;
  uint32_t chunks = 0;
  uint32_t retries = 0;
  int last_notify_error=0;
  const uint32_t started=millis();
  while (sent < length && isClientConnected(id, gen)) {
    const uint16_t chunk = static_cast<uint16_t>(
        std::min(max_payload,length - sent));
    // This API targets precisely one GATT connection; normal
    // BLECharacteristic::notify() broadcasts and must never be used here.
    // os_mbuf ownership transfers to NimBLE whether or not the controller
    // ultimately transmits the notification. The first argument is the
    // destination connection handle, never a broadcast list.
    uint16_t host_pending=0,host_queued=0;
    notifyCredit(id,host_pending,host_queued);
    const int pool_free=os_msys_num_free();
    os_mbuf* packet = ble_hs_mbuf_from_flat(data + sent, chunk);
    const int result=packet ? ble_gattc_notify_custom(
        id,tx_characteristic_->getHandle(),packet) : BLE_HS_ENOMEM;
    portENTER_CRITICAL(&mux_);
    stats_.host_pending_peak=std::max(stats_.host_pending_peak,host_pending);
    stats_.host_queue_peak=std::max(stats_.host_queue_peak,host_queued);
    stats_.msys_min_free=std::min<uint16_t>(stats_.msys_min_free,
        static_cast<uint16_t>(std::max(0,pool_free)));
    if(result!=0) {
      if(!packet) ++stats_.mbuf_alloc_failures;
      else ++stats_.notify_api_failures;
      stats_.memory_error_ms=millis();
      stats_.memory_error_conn=id;
      stats_.memory_error_code=result;
    }
    portEXIT_CRITICAL(&mux_);
    if (result!=0) {
      last_notify_error=result;
      // Two peers share NimBLE's bounded packet pool. Brief exhaustion is
      // backpressure, not permission to silently discard the ELM prompt.
      if ((result==BLE_HS_ENOMEM || result==BLE_HS_EAGAIN || result==BLE_HS_EBUSY) &&
          static_cast<uint32_t>(millis()-started)<100) {
        ++retries;
        delay(1);
        continue;
      }
      break;
    }
    sent += chunk;
    ++chunks;
    if (sent < length) delay(1);
  }
  portENTER_CRITICAL(&mux_);
  stats_.tx_bytes += sent;
  stats_.targeted_notifications += chunks;
  stats_.notify_retries += retries;
  if (sent<length) ++stats_.notify_failures;
  // Reconnect generation prevents old completions from polluting a new peer.
  for (auto& peer : clients_) {
    if (peer.active && peer.connection_id==id && peer.generation==gen) {
      peer.traffic.tx_bytes += sent;
      peer.traffic.notifications += chunks;
      peer.traffic.notify_retries += retries;
      if (sent<length) ++peer.traffic.notify_failures;
      peer.traffic.last_tx_ms = millis();
      break;
    }
  }
  if(retries)
    pushDiagnostic(BleDiagnosticEvent::Kind::Error,id,0,false,0,0,0,
                   static_cast<uint16_t>(DiagnosticError::BleNotifyBackpressure),
                   retries);
  if(sent<length)
    pushDiagnostic(BleDiagnosticEvent::Kind::Error,id,0,false,0,0,0,
                   static_cast<uint16_t>(DiagnosticError::BleNotifySendFailed),
                   static_cast<uint32_t>(last_notify_error));
  portEXIT_CRITICAL(&mux_);
  if (sent<length && isClientConnected(id,gen)) server_->disconnect(id);
  return sent;
}
#endif // M5CAN_SYNC_NOTIFY_DIAGNOSTIC
#endif

}  // namespace m5can
