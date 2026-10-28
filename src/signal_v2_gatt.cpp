#if defined(M5CAN_SIGNAL_V2_EXPERIMENTAL)
#include "signal_v2_gatt.h"
#include "nimble/nimble/host/include/host/ble_store.h"
#include "signal_v2_golden.h"
#include "signal_v2_registry.h"
#include "signal_v2_session.h"
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
#include "signal_v2_can_owner.h"
#endif
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <cstdio>
#include <cstring>
#include <mbedtls/sha256.h>
#if !defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL) && \
    (!defined(M5CAN_CAN_TX_HARD_DISABLED) || !M5CAN_CAN_TX_HARD_DISABLED)
#error "CAN-disabled V2-1 requires physical TX lock"
#endif
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL) && defined(M5CAN_CAN_TX_HARD_DISABLED)
#error "V2-2 TX-enabled image conflicts with V2-1 hard-disabled"
#endif
namespace signal_v2 {
namespace {
struct Link {
  uint16_t handle = 0;
  uint32_t generation = 0;
  bool active = false, status = false, data = false, data_busy = false;
  int confirm = 0;
};
Link links[2];
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
uint32_t generation = 0;
struct Incoming {
  uint16_t handle = 0;
  uint32_t generation = 0;
  size_t n = 0;
  uint8_t b[260]{};
};
QueueHandle_t inbox = nullptr, admin = nullptr;
NimBLECharacteristic *status = nullptr, *data = nullptr;
Registry registry;
Simulator simulator;
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
CanOwner owner;
bool can_armed=false;
uint32_t armed_until=0;
#endif
// Never inherit legacy V1/NVS DEMO or enable synthetic observations at boot.
bool demo_enabled = false;
unsigned active_slot = 0;
uint32_t boot = 0, window = 0, session_counter = 0;
bool window_open = false;
struct Peer {
  uint32_t generation = 0;
  Session session;
  Reassembler rx;
  Packet ack, output, input;
  size_t ackoff = 0, dataoff = 0;
  uint32_t transfer = 0, ack_transfer = 0, data_transfer = 0, sent = 0,
           progress = 0;
  bool waiting = false, authenticated = false;
};
Peer peers[2]; // fixed bounded storage; callbacks never parse or access session
Link snapshot(unsigned i) {
  portENTER_CRITICAL(&mux);
  auto l = links[i];
  portEXIT_CRITICAL(&mux);
  return l;
}
bool live(const Link &l) {
  bool ok = false;
  portENTER_CRITICAL(&mux);
  for (auto &x : links)
    if (x.active && x.handle == l.handle && x.generation == l.generation)
      ok = true;
  portEXIT_CRITICAL(&mux);
  return ok;
}
bool security(uint16_t handle, Identity &id, Binding &binding) {
  ble_gap_conn_desc d{};
  if (ble_gap_conn_find(handle, &d) || !d.sec_state.encrypted ||
      !d.sec_state.bonded)
    return false;
  id[0] = d.peer_id_addr.type;
  std::copy(d.peer_id_addr.val, d.peer_id_addr.val + 6, id.begin() + 1);
  ble_store_key_sec k{};
  k.peer_addr = d.peer_id_addr;
  ble_store_value_sec v{};
  // Match both the resolved identity and persistent key material. No key or
  // fingerprint appears in diagnostics. A replaced/missing bond fails closed.
  if (ble_store_read_peer_sec(&k, &v) || !v.ltk_present) {
    if (ble_store_read_our_sec(&k, &v) || !v.ltk_present)
      return false;
  }
  uint8_t material[55]{};
  std::copy(id.begin(), id.end(), material);
  std::copy(v.ltk, v.ltk + 16, material + 7);
  std::copy(v.irk, v.irk + 16, material + 23);
  material[39] = v.key_size;
  material[40] = v.sc;
  material[41] = v.authenticated;
  mbedtls_sha256_ret(material, sizeof(material), binding.data(), 0);
  std::memset(&v, 0, sizeof(v));
  std::memset(material, 0, sizeof(material));
  return true;
}
bool authorized(const Link &l) {
  Identity id;
  Binding b;
  return live(l) && security(l.handle, id, b) && registry.authorized(id, b);
}
bool persist(const Registry &next) {
  Preferences p;
  if (!p.begin("m5can_sigv2", false))
    return false;
  const char *key = active_slot ? "a" : "b";
  RegistryImage check{};
  bool ok = p.putBytes(key, next.image().data(), 256) == 256 &&
            p.getBytes(key, check.data(), 256) == 256 && check == next.image();
  p.end();
  if (ok) {
    registry = next;
    active_slot ^= 1;
  }
  return ok;
}
class Callbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, ble_gap_conn_desc *d) override {
    if (!d || !d->sec_state.encrypted || !d->sec_state.bonded)
      return;
    auto v = c->getValue();
    if (!v.size() || v.size() > 260)
      return;
    Incoming in;
    in.handle = d->conn_handle;
    in.n = v.size();
    std::copy(v.begin(), v.end(), in.b);
    for (unsigned i = 0; i < 2; ++i) {
      auto l = snapshot(i);
      if (l.active && l.handle == in.handle) {
        in.generation = l.generation;
        if (xQueueSend(inbox, &in, 0) != pdTRUE)
          NimBLEDevice::getServer()->disconnect(in.handle);
        return;
      }
    }
  }
  void onSubscribe(NimBLECharacteristic *c, ble_gap_conn_desc *d,
                   uint16_t sub) override {
    if (!d)
      return;
    portENTER_CRITICAL(&mux);
    for (auto &l : links)
      if (l.active && l.handle == d->conn_handle) {
        if (c == status)
          l.status = (sub & 2) != 0;
        else if (c == data)
          l.data = (sub & 1) != 0;
      }
    portEXIT_CRITICAL(&mux);
  }
};
void manage(const char *cmd, uint32_t now) {
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
  if(!std::strcmp(cmd,"SIGV2 CAN STOP")) {
    can_armed=false;owner.invalidate(10);
    for(auto &p:peers) {
      p.output.size=p.dataoff=0;
      p.session.clearDemoCache(now);
    }
    Serial.println("#SIGNALV2_CAN,armed=0,reason=OPERATOR_STOP");
    return;
  }
  if(!std::strcmp(cmd,"SIGV2 CAN ARM")) {
    bool secured=false;
    for(unsigned i=0;i<2;++i){
      auto l=snapshot(i);
      if(l.active&&l.status&&l.data&&authorized(l)&&
         peers[i].session.active(now)) secured=true;
    }
    can_armed=secured && !owner.stopped();
    armed_until=now+600000;
    Serial.printf("#SIGNALV2_CAN,armed=%u,reason=%s,ttl_ms=600000\n",
      can_armed?1U:0U,can_armed?"USB_VERIFIED":"NOT_READY");
    return;
  }
  if(!std::strncmp(cmd,"SIGV2 DEMO ",11)) {
    Serial.println("#SIGNALV2_CAN,admin=DEMO_UNAVAILABLE");
    return;
  }
#endif
  if (!std::strcmp(cmd, "SIGV2 DEMO STATUS") ||
      !std::strcmp(cmd, "SIGV2 DEMO ON") ||
      !std::strcmp(cmd, "SIGV2 DEMO OFF")) {
    if (!std::strcmp(cmd, "SIGV2 DEMO ON") ||
        !std::strcmp(cmd, "SIGV2 DEMO OFF")) {
      const bool desired = !std::strcmp(cmd, "SIGV2 DEMO ON");
      if (demo_enabled != desired) {
        demo_enabled = desired;
        // Clear validity but retain per-PID monotonic sequence across toggles.
        simulator.tick(now, Simulator::Demand{});
        for (auto &peer : peers) {
          peer.session.clearDemoCache(now);
          // Prevent transmitting any queued pre-transition demo observations.
          peer.output.size = peer.dataoff = 0;
        }
      }
    }
    Serial.printf("#SIGNALV2,synthetic_demo=%s,usb_only=1,can_tx_disabled=1,volatile=1\n",
                  demo_enabled ? "ON" : "OFF");
    return;
  }
  if (!std::strcmp(cmd, "SIGV2 GOLDEN")) {
    auto result = golden();
    for (unsigned i = 0; i < 12; ++i)
      Serial.printf("#SIGNALV2_GOLDEN,G%02u=%s,can_tx_disabled=1\n", i + 1,
                    result[i] ? "PASS" : "FAIL");
    return;
  }
  if (!std::strcmp(cmd, "SIGV2 WINDOW")) {
    window = now + 90000;
    window_open = true;
    Serial.println("#SIGNALV2,registration_window=90s,usb_verified_required=1");
    return;
  }
  if (!std::strcmp(cmd, "SIGV2 PEERS")) {
    for (unsigned i = 0; i < 2; ++i) {
      auto l = snapshot(i);
      Identity id;
      Binding bind;
      if (l.active && security(l.handle, id, bind))
        Serial.printf("#SIGNALV2_ADMIN_PRIVATE,usb_verified_candidate=%02X:%02X:%02X:%02X:%"
                      "02X:%02X,registered=%u\n",
                      id[6], id[5], id[4], id[3], id[2], id[1],
                      registry.authorized(id, bind) ? 1 : 0);
    }
    return;
  }
  bool approve = !std::strncmp(cmd, "SIGV2 APPROVE ", 14),
       revoke = !std::strncmp(cmd, "SIGV2 REVOKE ", 13);
  if (!approve && !revoke) {
    Serial.printf("#SIGNALV2,git=%s,target=M5Dial,can_tx_"
                  "disabled=1,physical_tx=0,registry_"
                  "generation=%lu,heap=%u,stack=%u\n",
                  M5CAN_BUILD_SHA,
                  (unsigned long)registry.generation(), ESP.getFreeHeap(),
                  uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t));
    return;
  }
  unsigned b[6];
  const char *a = cmd + (approve ? 14 : 13);
  int used = 0;
  if (std::sscanf(a, "%2x:%2x:%2x:%2x:%2x:%2x%n", b, b + 1, b + 2, b + 3, b + 4,
                  b + 5, &used) != 6 ||
      a[used]) {
    Serial.println("#SIGNALV2,admin=BAD_IDENTITY");
    return;
  }
  if (approve && (!window_open || int32_t(window - now) <= 0)) {
    Serial.println("#SIGNALV2,admin=WINDOW_CLOSED");
    return;
  }
  if (revoke) {
    for (unsigned i = 0; i < 4; ++i) {
      auto r = registry.image().data() + 16 + 56 * i;
      Identity id;
      std::copy(r + 2, r + 9, id.begin());
      bool matches = r[0] == 1;
      for (unsigned j = 0; j < 6; ++j)
        matches &= id[1 + j] == b[5 - j];
      if (matches) {
        Registry next = registry;
        bool ok = next.revoke(id) && persist(next);
        Serial.printf("#SIGNALV2,admin=REVOKE,saved=%u\n", ok ? 1 : 0);
        return;
      }
    }
    Serial.println("#SIGNALV2,admin=NO_REGISTERED_MATCH");
    return;
  }
  for (unsigned i = 0; i < 2; ++i) {
    auto l = snapshot(i);
    Identity id;
    Binding bind;
    if (!l.active || !security(l.handle, id, bind))
      continue;
    bool matches = true;
    for (unsigned j = 0; j < 6; ++j)
      matches &= id[1 + j] == b[5 - j];
    if (!matches)
      continue;
    Registry next = registry;
    bool ok =
        (approve ? next.approve(id, bind) : next.revoke(id)) && persist(next);
    if (ok) {
      peers[i].session.connect(boot, ++session_counter);
      peers[i].ack.size = peers[i].output.size = 0;
      peers[i].rx.reset();
      window_open = false;
    }
    Serial.printf(
        "#SIGNALV2,admin=%s,saved=%u,permission=CONNECT_SIGNAL_DEMO_ONLY\n",
        approve ? "APPROVE" : "REVOKE", ok ? 1 : 0);
    return;
  }
  Serial.println("#SIGNALV2,admin=NO_SECURED_MATCH");
}
void send(Peer &p, const Link &l, uint32_t now) {
  if (!live(l))
    return;
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
  if(p.output.size && (p.output.bytes[4]==0x20 ||
                       p.output.bytes[4]==0x21) &&
     !p.session.canPacketCurrent(now,p.output,owner.samples())){
    p.output.size=0;
    p.dataoff=0;
    p.session.clearDemoCache(now);
    Serial.println("#SIGNALV2_CAN,queue_drop=STALE_OR_INVALID,quality=INVALID");
  }
#endif
  if (p.waiting) {
    int confirmed = 0;
    portENTER_CRITICAL(&mux);
    for (auto &x : links)
      if (x.active && x.handle == l.handle && x.generation == l.generation) {
        confirmed = x.confirm;
        x.confirm = 0;
      }
    portEXIT_CRITICAL(&mux);
    if (confirmed == 1) {
      p.waiting = false;
      p.ackoff += p.sent;
      if (p.ackoff >= p.ack.size) {
        p.ack.size = 0;
        p.ackoff = 0;
      }
    } else if (confirmed < 0 || uint32_t(now - p.progress) >= 2000) {
      p.session.invalidate();
      p.ack.size = p.output.size = 0;
      p.waiting = false;
      NimBLEDevice::getServer()->disconnect(l.handle);
    }
    return;
  }
  bool isack = p.ack.size != 0;
  if (!isack && l.data_busy) {
    if (uint32_t(now - p.progress) > 2000)
      NimBLEDevice::getServer()->disconnect(l.handle);
    return;
  }
  Packet &packet = isack ? p.ack : p.output;
  size_t &off = isack ? p.ackoff : p.dataoff;
  if (!packet.size)
    return;
  if ((isack && !l.status) || (!isack && !l.data)) {
    p.session.invalidate();
    packet.size = 0;
    off = 0;
    return;
  }
  // One fragment/peer/poll, never preload a stream behind another peer.
  // A notify completion event paces DATA; indication confirmation paces ACK.
  ble_gap_conn_desc d{};
  if (ble_gap_conn_find(l.handle, &d))
    return;
  uint8_t f[260];
  uint16_t mtu = ble_att_mtu(l.handle);
  size_t n = fragment(packet, isack ? 3 : 2,
                      isack ? p.ack_transfer : p.data_transfer, off, mtu, f);
  if (!n)
    return;
  os_mbuf *mb = ble_hs_mbuf_from_flat(f, n);
  if (!mb) {
    // Allocation failure has the same bounded no-progress deadline as API
    // backpressure; an ACK must not remain pending indefinitely.
    if (uint32_t(now - p.progress) >= 2000) {
      p.session.invalidate();
      NimBLEDevice::getServer()->disconnect(l.handle);
    }
    return;
  }
  if (!isack) {
    portENTER_CRITICAL(&mux);
    for (auto &x : links)
      if (x.active && x.handle == l.handle && x.generation == l.generation)
        x.data_busy = true;
    portEXIT_CRITICAL(&mux);
  }
  int rc = isack ? ble_gattc_indicate_custom(l.handle, status->getHandle(), mb)
                 : ble_gattc_notify_custom(l.handle, data->getHandle(), mb);
  if (rc) {
    if (!isack) {
      portENTER_CRITICAL(&mux);
      for (auto &x : links)
        if (x.active && x.handle == l.handle && x.generation == l.generation)
          x.data_busy = false;
      portEXIT_CRITICAL(&mux);
    }
    if (uint32_t(now - p.progress) > 2000) {
      p.session.invalidate();
      NimBLEDevice::getServer()->disconnect(l.handle);
    }
    return;
  }
  p.progress = now;
  if (isack) {
    p.waiting = true;
    p.sent = n - 16;
  } else {
    off += n - 16;
    if (off >= packet.size) {
      packet.size = 0;
      off = 0;
      ++p.transfer;
    }
  }
}
} // namespace
bool begin(NimBLEServer *server) {
  inbox = xQueueCreate(12, sizeof(Incoming));
  admin = xQueueCreate(4, 129);
  if (!inbox || !admin)
    return false;
  Preferences nvs;
  RegistryImage a{}, b{};
  if (nvs.begin("m5can_sigv2", true)) {
    nvs.getBytes("a", a.data(), 256);
    nvs.getBytes("b", b.data(), 256);
    nvs.end();
  }
  registry.load(a, b);
  active_slot = (registry.image() == b) ? 1 : 0;
  session_counter = esp_random() & 0x7fffffff;
  boot = esp_random();
  if (!boot)
    boot = 1;
  NimBLEDevice::setSecurityAuth(true, false, true);
  // Full bond table must reject pairing, never evict an existing owner's key.
  ble_hs_cfg.store_status_cb = [](ble_store_status_event *, void *) {
    return BLE_HS_ESTORE_CAP;
  };
  auto service = server->createService(Service);
  if (!service)
    return false;
  auto control = service->createCharacteristic(
      Control, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
  data = service->createCharacteristic(Data, NIMBLE_PROPERTY::NOTIFY |
                                                 NIMBLE_PROPERTY::READ_ENC);
  status = service->createCharacteristic(Status, NIMBLE_PROPERTY::READ |
                                                     NIMBLE_PROPERTY::INDICATE |
                                                     NIMBLE_PROPERTY::READ_ENC);
  if (!control || !data || !status)
    return false;
  auto cb = new Callbacks;
  control->setCallbacks(cb);
  data->setCallbacks(cb);
  status->setCallbacks(cb);
  uint8_t safe[12] = {0};
  w32(safe + 8, 1);
  auto state = encode(0x22, boot, 0, 0, safe, 12);
  status->setValue(state.bytes.data(),
                   state.size); // Read never leaks another peer's ACK.
  service->start();
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
  Serial.printf("#SIGNALV2,boot=%lu,mode=CAN,armed=0,usb_arm_required=1\n",
                (unsigned long)boot);
#else
  Serial.printf(
      "#SIGNALV2,boot=%lu,mode=DEMO,can_tx_disabled=1,physical_tx=0\n",
      (unsigned long)boot);
#endif
  return true;
}
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
void attachCan(m5can::CanMonitor* m){owner.attach(m);}
#endif
void connected(uint16_t handle) {
  portENTER_CRITICAL(&mux);
  for (auto &l : links)
    if (!l.active) {
      l = Link{};
      l.active = true;
      l.handle = handle;
      l.generation = ++generation;
      break;
    }
  portEXIT_CRITICAL(&mux);
}
void disconnected(uint16_t handle) {
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
  can_armed=false; // New connection requires a new local USB arm.
  owner.invalidate(10);
#endif
  portENTER_CRITICAL(&mux);
  for (auto &l : links)
    if (l.active && l.handle == handle) {
      l.active = false;
      l.status = l.data = false;
    }
  portEXIT_CRITICAL(&mux);
}
void gap(ble_gap_event *e) {
  if (!e || !status || e->type != BLE_GAP_EVENT_NOTIFY_TX)
    return;
  portENTER_CRITICAL(&mux);
  for (auto &l : links)
    if (l.active && l.handle == e->notify_tx.conn_handle) {
      if (e->notify_tx.indication &&
          e->notify_tx.attr_handle == status->getHandle() &&
          e->notify_tx.status)
        l.confirm = e->notify_tx.status == BLE_HS_EDONE ? 1 : -1;
      else if (!e->notify_tx.indication &&
               e->notify_tx.attr_handle == data->getHandle())
        l.data_busy = false;
    }
  portEXIT_CRITICAL(&mux);
}
bool usbCommand(const char *command) {
  if (std::strncmp(command, "SIGV2 ", 6))
    return false;
  char copy[129]{};
  std::strncpy(copy, command, 128);
  if (!admin || xQueueSend(admin, copy, 0) != pdTRUE)
    Serial.println("#SIGNALV2,admin=BUSY");
  return true;
}
void poll() {
  if (!inbox)
    return;
  uint32_t now = millis();
  char cmd[129];
  if (xQueueReceive(admin, cmd, 0) == pdTRUE)
    manage(cmd, now);
  Simulator::Demand demand{};
  for (unsigned i = 0; i < 2; ++i) {
    auto link = snapshot(i);
    if (link.active && link.status && link.data && authorized(link))
      peers[i].session.demand(now, demand);
  }
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
  if(can_armed && int32_t(armed_until-now)<=0){
    can_armed=false;
    Serial.println("#SIGNALV2_CAN,armed=0,reason=USB_ARM_EXPIRED");
  }
  owner.tick(now,demand,can_armed);
  static bool last_unsafe=false;
  static uint32_t last_acquired=0,last_invalid=0;
  const bool unsafe=!can_armed || owner.stopped();
  const auto &samples=owner.samples();
  const bool source_transition=samples.acquired!=last_acquired ||
                               samples.invalidations!=last_invalid;
  // Never deliver an older queued CAN PDU after a newer acquisition, timeout,
  // STOP or source invalidation. Snapshot cannot resurrect pre-error records.
  if((unsafe && !last_unsafe) || (!unsafe && source_transition)){
    for(auto &p:peers){
      p.output.size=p.dataoff=0;
      p.session.clearDemoCache(now);
    }
  }
  last_acquired=samples.acquired;last_invalid=samples.invalidations;
  last_unsafe=unsafe;
  simulator.tick(now,Simulator::Demand{});
#else
  simulator.tick(now, demo_enabled ? demand : Simulator::Demand{});
#endif
  for (unsigned i = 0; i < 2; ++i) {
    auto l = snapshot(i);
    auto &p = peers[i];
    if (!l.active) {
      p.session.invalidate();
      p.ack.size = p.output.size = 0;
      p.waiting = false;
      p.rx.reset();
      continue;
    }
    if (p.generation != l.generation) {
      p.generation = l.generation;
      p.session.connect(boot, ++session_counter);
      p.authenticated = false;
      p.rx.reset();
      p.ack.size = p.output.size = 0;
      p.ackoff = p.dataoff = 0;
      p.waiting = false;
      p.progress = now;
      ++p.transfer;
    }
    bool auth = authorized(l) && l.status && l.data;
    if (auth != p.authenticated) {
      p.session.connect(boot, ++session_counter);
      p.authenticated = auth;
      p.ack.size = p.output.size = 0;
      p.ackoff = p.dataoff = 0;
      p.waiting = false;
      p.rx.reset();
    }
    if (!auth) {
      p.session.invalidate();
      p.output.size = 0;
    }
    // A queued pre-expiry DATA/SNAPSHOT must not be transmitted after lease
    // expiry, even when a notify fragment was previously backpressured.
    if (!p.session.active(now) && p.output.size &&
        (p.output.bytes[4] == 0x20 || p.output.bytes[4] == 0x21)) {
      p.output.size = 0;
      p.dataoff = 0;
    }
    if (!p.ack.size && !p.output.size && l.status && l.data && authorized(l) &&
        p.session.produce(now, p.output, &simulator, demo_enabled
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
                          , &owner.samples()
#endif
                          )) {
      p.dataoff = 0;
      p.data_transfer = ++p.transfer;
    }
    send(p, l, now);
  }
  // Parse at most one complete callback event, always rechecking generation.
  Incoming in;
  if (xQueueReceive(inbox, &in, 0) != pdTRUE)
    return;
  for (unsigned i = 0; i < 2; ++i) {
    auto l = snapshot(i);
    auto &p = peers[i];
    if (!l.active || l.handle != in.handle || l.generation != in.generation)
      continue;
    if (p.ack.size)
      return;
    auto e = p.rx.feed(in.b, in.n, 1, now, p.input);
    if (e == Error::None) {
      p.ackoff = 0;
      p.ack_transfer = ++p.transfer;
      p.progress = now;
      p.session.control(p.input, authorized(l) && l.status && l.data, now,
                        p.ack);
      auto op = p.input.payload()[8];
      if (p.ack.size && p.ack.payload()[5] == Accept &&
          (op == 2 || op == 3 || op == 5 || op == 6 || op == 7 || op == 10)) {
        p.output.size = 0;
        p.dataoff = 0;
      }
    }
    return;
  }
}
} // namespace signal_v2
#endif
