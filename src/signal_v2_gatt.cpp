#include <atomic>
#if defined(M5CAN_SIGNAL_V2_EXPERIMENTAL)
#include "signal_v2_gatt.h"
#include "nimble/nimble/host/include/host/ble_store.h"
#include "signal_v2_golden.h"
#include "signal_v2_registry.h"
#include "signal_v2_session.h"
#include "signal_v2_disconnect_owner.h"
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
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL) && \
    !defined(M5CAN_SIGNAL_V2_STAGE_GATED) && !defined(M5CAN_SIGNAL_V2_SERIAL_HIL) && \
    !(defined(M5CAN_SIGNAL_V2_OWNER_CONTINUOUS_LIVE) && \
      M5CAN_SIGNAL_V2_OWNER_CONTINUOUS_LIVE == 1 && \
      defined(M5CAN_SIGNAL_V2_RP8_PROFILE) && M5CAN_SIGNAL_V2_RP8_PROFILE == 1)
#error "Vehicle V2 requires STAGE_GATED or explicit RP8 OWNER_CONTINUOUS_LIVE=1"
#endif
#if defined(M5CAN_SIGNAL_V2_SERIAL_HIL) && defined(M5CAN_SIGNAL_V2_STAGE_GATED)
#error "Serial-HIL ARM/GROUP requires an unstaged, ELF-isolated USB-only image"
#endif
#if defined(M5CAN_SIGNAL_V2_OWNER_CONTINUOUS_LIVE) && \
    (M5CAN_SIGNAL_V2_OWNER_CONTINUOUS_LIVE != 1 || \
     !defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL) || \
     !defined(M5CAN_SIGNAL_V2_RP8_PROFILE) || M5CAN_SIGNAL_V2_RP8_PROFILE != 1 || \
     defined(M5CAN_SIGNAL_V2_SERIAL_HIL) || defined(M5CAN_SIGNAL_V2_STAGE_GATED) || \
     defined(M5CAN_SIGNAL_V2_10HZ_LAB))
#error "OWNER_CONTINUOUS_LIVE is exclusively unstaged physical RP8 5Hz"
#endif
#if defined(M5CAN_SIGNAL_V2_OWNER_AUTO_ARM) && \
    (M5CAN_SIGNAL_V2_OWNER_AUTO_ARM != 1 || \
     !defined(M5CAN_SIGNAL_V2_OWNER_CONTINUOUS_LIVE) || \
     M5CAN_SIGNAL_V2_OWNER_CONTINUOUS_LIVE != 1)
#error "OWNER_AUTO_ARM requires the exclusive OWNER_CONTINUOUS_LIVE image"
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
DisconnectOwner disconnect_owner, arm_owner;
std::atomic<bool> can_armed{false};
std::atomic<uint32_t> can_authorization_epoch{1};
uint32_t armed_until=0;
#if defined(M5CAN_SIGNAL_V2_OWNER_AUTO_ARM)
std::atomic<bool> manual_stop{false};
std::atomic<bool> manual_stop_pending{false};
struct AutoArmSession {
  bool tracking=false;
  unsigned slot=0;
  uint32_t generation=0, session=0, epoch=0, since=0;
} auto_session;
#endif
#if defined(M5CAN_SIGNAL_V2_OWNER_CONTINUOUS_LIVE)
// Published only by the BLE application task; LCD never reads owner storage.
std::atomic<bool> display_busy{false};
std::atomic<const char*> display_reason{nullptr};
#endif
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
// Sticky per-slot notification: independent of the already-full Control FIFO.
std::atomic<unsigned> control_revoke_pending{0};
void revokeControlLink(uint16_t handle) {
  unsigned slots=0;
  portENTER_CRITICAL(&mux);
  for(unsigned i=0;i<2;++i) {
    auto &l=links[i];
    if(l.active && l.handle==handle) {
      l.active=l.status=l.data=false;
      slots|=1U<<i;
    }
  }
  portEXIT_CRITICAL(&mux);
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
  const uint32_t revoked=++can_authorization_epoch;
  can_armed=false;
  owner.revokeAuthorizationAsync(revoked);
#endif
  control_revoke_pending.fetch_or(slots);
}
void serviceControlRevocations(uint32_t now) {
  const unsigned slots=control_revoke_pending.exchange(0);
  if(!slots)return;
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
  owner.noteAuthorizationEnd("CONTROL_FIFO_OVERFLOW");
  owner.invalidate(10);
#endif
  for(unsigned i=0;i<2;++i) {
    auto &p=peers[i];
    // All queued CAN output belongs to the revoked global authorization.
    p.output.size=p.dataoff=0;
    p.session.clearDemoCache(now);
    if(slots&(1U<<i)) {
      p.session.invalidate();
      p.ack.size=p.ackoff=0;
      p.waiting=false;
      p.rx.reset();
    }
  }
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
        if (xQueueSend(inbox, &in, 0) != pdTRUE) {
          // No room to retain this Control: transport protocol error. Do not
          // overwrite the pending ACK or pretend the request was accepted.
          revokeControlLink(in.handle);
          Serial.println("#SIGNALV2,protocol_error=CONTROL_FIFO_OVERFLOW");
          NimBLEDevice::getServer()->disconnect(in.handle);
        }
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
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
// Shared USB/automatic grant: snapshot before validation, CAS, publish, post-check.
bool grantCanArm(uint32_t now,uint32_t seen) {
    bool secured=false;
    DisconnectOwner binding;
    unsigned candidates=0;
    for(unsigned i=0;i<2;++i){
      auto l=snapshot(i);
      if(l.active&&l.status&&l.data&&authorized(l)&&
         peers[i].session.active(now)) {
        secured=true;++candidates;
        binding.handle=l.handle;binding.generation=l.generation;
        binding.session=peers[i].session.id();
      }
    }
#if defined(M5CAN_SIGNAL_V2_OWNER_AUTO_ARM)
    secured=secured && !manual_stop.load() && !owner.stopReason();
#endif
    uint32_t expected=seen;
    bool accepted=secured &&
      can_authorization_epoch.compare_exchange_strong(expected,seen+1);
    can_armed=accepted;
    armed_until=now+600000;
    // A disconnect after the grant must also withdraw a flag stored after
    // its revocation. No later poll is needed to make this ARM invalid.
    if(accepted && (can_authorization_epoch.load()!=seen+1
#if defined(M5CAN_SIGNAL_V2_OWNER_AUTO_ARM)
       || manual_stop.load() || owner.stopReason()
#endif
       )){
      can_armed=false;accepted=false;
    }
    binding.known=accepted && candidates==1;
    binding.epoch=seen+1;
    arm_owner=binding; // BLE owner task only; callback uses the published copy.
    return accepted;
}
#endif
void manage(const char *cmd, uint32_t now) {
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
  if(!std::strcmp(cmd,"SIGV2 CAN STOP")) {
#if defined(M5CAN_SIGNAL_V2_OWNER_AUTO_ARM)
    manual_stop=true;
#endif
    owner.noteAuthorizationEnd("OPERATOR_STOP");
    can_armed=false;const uint32_t revoked=++can_authorization_epoch;
    owner.revokeAuthorization(revoked);owner.invalidate(10);
#if defined(M5CAN_SIGNAL_V2_SERIAL_HIL)
    hil::note("kind=stop_request,epoch=%lu",(unsigned long)can_authorization_epoch.load());
#endif
    for(auto &p:peers) {
      p.output.size=p.dataoff=0;
      p.session.clearDemoCache(now);
    }
    Serial.println("#SIGNALV2_CAN,armed=0,reason=OPERATOR_STOP,ack=REQUESTED");
    return;
  }
#if defined(M5CAN_SIGNAL_V2_SERIAL_HIL)
  if(!std::strcmp(cmd,"SIGV2 CAN READY")) {
    bool ready=false;
    for(unsigned i=0;i<2;++i){
      auto l=snapshot(i);
      if(l.active&&l.status&&l.data&&authorized(l)&&peers[i].session.active(now))ready=true;
    }
    hil::note("kind=ready,ok=%u",ready?1U:0U);
    return;
  }
#endif
  if(!std::strcmp(cmd,"SIGV2 CAN ARM")) {
#if defined(M5CAN_SIGNAL_V2_STAGE_GATED)
    // Owner decision O-1 (review B-02): in the vehicle-capable staged build a
    // generic ARM is not a vehicle-stage authorization. S1 has its own
    // command; S2/S3 stay unreachable until their explicit stages exist.
    Serial.println("#SIGNALV2_CAN,armed=0,reason=STAGE_AUTHORIZATION_REQUIRED");
    return;
#endif
#if defined(M5CAN_SIGNAL_V2_OWNER_AUTO_ARM)
    if(owner.stopReason()) {
      Serial.println("#SIGNALV2_CAN,armed=0,reason=FAULT_LATCHED");
      return;
    }
    manual_stop=false;
    auto_session={};
#endif
    grantCanArm(now,can_authorization_epoch.load());
#if defined(M5CAN_SIGNAL_V2_SERIAL_HIL)
    hil::note("kind=arm,ok=%u,epoch=%lu",can_armed?1U:0U,(unsigned long)can_authorization_epoch.load());
#endif
    Serial.printf("#SIGNALV2_CAN,armed=%u,reason=%s,ttl_ms=600000\n",
      can_armed?1U:0U,can_armed?"USB_VERIFIED":"NOT_READY");
    return;
  }
  // PR-C2 stage S1 (owner decision O-1): its own explicit authorization,
  // never implied by ARM. Requires the same secured central and an idle
  // owner; bounded by the 1s observation plus the 3s fail-safe.
  if(!std::strcmp(cmd,"SIGV2 CAN S1")) {
    // Snapshot the epoch before checking the secured link: a disconnect at
    // any later point bumps it, and the compare-exchange below then refuses
    // to authorize (review B-01, linearizes check -> grant).
    const uint32_t seen=can_authorization_epoch.load();
    bool secured=false;
    DisconnectOwner binding;
    unsigned candidates=0;
    for(unsigned i=0;i<2;++i){
      auto l=snapshot(i);
      if(l.active&&l.status&&l.data&&authorized(l)&&
         peers[i].session.active(now)) {
        secured=true;++candidates;
        binding.handle=l.handle;binding.generation=l.generation;
        binding.session=peers[i].session.id();
      }
    }
    bool accepted=secured && !can_armed && !owner.busy();
    uint32_t expected=seen;
    if(accepted && !can_authorization_epoch.compare_exchange_strong(expected,seen+1))
      accepted=false; // a disconnect/STOP intervened after the snapshot
    if(accepted){
      // Bind the request to the epoch it creates; a disconnect after this
      // point bumps the epoch, so the request is never consumed and the
      // stale arm is withdrawn (review F1).
      const uint32_t epoch=seen+1;
      owner.requestStageS1(epoch);
      can_armed=true;
      armed_until=now+5000;
      if(can_authorization_epoch.load()!=epoch){can_armed=false;accepted=false;}
      binding.known=accepted && candidates==1;binding.epoch=epoch;
      arm_owner=binding;
    }
    Serial.printf("#SIGNALV2_CAN,stage=S1,armed=%u,reason=%s,ttl_ms=5000\n",
      accepted?1U:0U,accepted?"USB_VERIFIED":(secured?"BUSY_OR_ARMED":"NOT_READY"));
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
#if defined(M5CAN_SIGNAL_V2_SERIAL_HIL)
  if(!isack && off==0 && (packet.bytes[4]==0x20 || packet.bytes[4]==0x21)){
    const uint8_t *payload=packet.payload();
    const unsigned records=packet.bytes[4]==0x21?payload[12]:payload[0];
    size_t at=packet.bytes[4]==0x21?20:2;unsigned fresh=0;
    // Generations actually carried by the notified Fresh records (lowest
    // value), independent of the owner's current counters (review B-03).
    uint32_t pdu_generation=0,pdu_mode=0,pdu_source=0;bool pdu_mixed=false;
    for(unsigned i=0;i<records && at+44<=packet.size-Header;++i){
      if(payload[at+7]<=1){
        const uint32_t g=u32(payload+at+14),m=u32(payload+at+18),src=u32(payload+at+22);
        if(fresh && (g!=pdu_generation||m!=pdu_mode||src!=pdu_source))pdu_mixed=true;
        if(!fresh||g<pdu_generation)pdu_generation=g;
        if(!fresh||m<pdu_mode)pdu_mode=m;
        pdu_source=src;++fresh;
      }
      at+=u16(payload+at);
    }
    hil::note("kind=packet,type=%u,epoch=%lu,fresh=%u,current=%u,source_generation=%lu,mode_epoch=%lu,"
      "pdu_source_generation=%lu,pdu_mode_epoch=%lu,pdu_source=%08lX,pdu_mixed=%u",
      unsigned(packet.bytes[4]),(unsigned long)can_authorization_epoch.load(),fresh,
      p.session.canPacketCurrent(now,packet,owner.samples())?1U:0U,
      (unsigned long)owner.samples().source_generation,(unsigned long)owner.samples().mode_epoch,
      (unsigned long)pdu_generation,(unsigned long)pdu_mode,(unsigned long)pdu_source,pdu_mixed?1U:0U);
  }
#endif
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
#if defined(M5CAN_SIGNAL_V2_OWNER_AUTO_ARM)
  Serial.printf("#SIGNALV2,boot=%lu,mode=CAN,armed=0,auto_arm_settle_ms=2000\n",
                (unsigned long)boot);
#else
  Serial.printf("#SIGNALV2,boot=%lu,mode=CAN,armed=0,usb_arm_required=1\n",
                (unsigned long)boot);
#endif
#else
  Serial.printf(
      "#SIGNALV2,boot=%lu,mode=DEMO,can_tx_disabled=1,physical_tx=0\n",
      (unsigned long)boot);
#endif
  return true;
}
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
void attachCan(m5can::CanMonitor* m){owner.attach(m);}
#if defined(M5CAN_SIGNAL_V2_OWNER_CONTINUOUS_LIVE)
CanTxDisplay canTxDisplay(){
  return {can_armed.load(),display_busy.load(),display_reason.load()
#if defined(M5CAN_SIGNAL_V2_OWNER_AUTO_ARM)
    ,manual_stop.load()
#else
    ,false
#endif
  };
}
#endif
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
  bool known_peer=false, owner_live=false;
#endif
  portENTER_CRITICAL(&mux);
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
  // Only a stable, unexpired owner publication can prove a non-owner.
  if(can_armed.load() && disconnect_owner.current(can_authorization_epoch.load(),millis()))
    for(const auto &l:links)
      if(l.active && l.status && l.data && l.handle==disconnect_owner.handle &&
         l.generation==disconnect_owner.generation) owner_live=true;
  for(const auto &l:links)
    if(l.active && l.handle==handle) known_peer=true;
  const bool non_owner=known_peer && owner_live && handle!=disconnect_owner.handle;
#endif
  for (auto &l : links)
    if (l.active && l.handle == handle) {
      l.active = false;
      l.status = l.data = false;
    }
  portEXIT_CRITICAL(&mux);
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
  if(non_owner)return;
#if defined(M5CAN_SIGNAL_V2_SERIAL_HIL)
  hil::note("kind=disconnect,handle=%u",unsigned(handle));
#endif
  owner.noteAuthorizationEnd("CENTRAL_DISCONNECT");
  // Invalidate the link before publishing a new epoch: an ARM observing
  // that epoch must not validate this disconnected link. Publish revocation
  // before clearing the flag so a concurrent grant cannot survive the clear.
  const uint32_t revoked=++can_authorization_epoch;
  can_armed=false; // A new authorization epoch is required after reconnect.
  owner.revokeAuthorization(revoked); // Thread-safe lease revoke; no sample/selector mutation.
#endif
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
#if defined(M5CAN_SIGNAL_V2_OWNER_AUTO_ARM)
// UI thread: revoke immediately, defer session/cache mutation to BLE app owner.
void manualCanStop() {
  manual_stop=true;
  manual_stop_pending=true;
  const uint32_t revoked=++can_authorization_epoch;
  can_armed=false;
  owner.revokeAuthorization(revoked);
}
void serviceAutoArm(uint32_t now) {
  const uint32_t seen=can_authorization_epoch.load();
  if(manual_stop.load() || owner.stopReason()) {
    auto_session={};
    return;
  }
  // Bind to one exact link generation and protocol session, never aggregate peers.
  int candidate=-1;
  for(unsigned i=0;i<2;++i) {
    const auto l=snapshot(i);
    if(l.active && l.status && l.data && authorized(l) &&
       peers[i].generation==l.generation && peers[i].session.active(now)) {
      if(!auto_session.tracking || (auto_session.slot==i &&
         auto_session.generation==l.generation &&
         auto_session.session==peers[i].session.id())) {candidate=int(i);break;}
    }
  }
  if(candidate<0 || (auto_session.tracking && auto_session.epoch!=seen)) {
    auto_session={};
    if(can_armed) {
      owner.noteAuthorizationEnd("SESSION_ENDED");
      const uint32_t revoked=++can_authorization_epoch;
      can_armed=false;
      owner.revokeAuthorization(revoked);owner.invalidate(10);
    }
    return; // a replacement session starts a fresh settle window next tick
  }
  if(!auto_session.tracking) {
    const auto l=snapshot(unsigned(candidate));
    auto_session.tracking=true;
    auto_session.slot=unsigned(candidate);
    auto_session.generation=l.generation;
    auto_session.session=peers[candidate].session.id();
    auto_session.epoch=seen;
    auto_session.since=now;
  }
  if(can_armed) {
    // Only the bound, still-secured active session can extend the USB ARM TTL.
    armed_until=now+600000;
  } else if(uint32_t(now-auto_session.since)>=2000 && !owner.busy()) {
    if(grantCanArm(now,seen))auto_session.epoch=seen+1;
  }
}
#endif
bool usbCommand(const char *command) {
  if (std::strncmp(command, "SIGV2 ", 6))
    return false;
  char copy[129]{};
  std::strncpy(copy, command, 128);
  if (!admin || xQueueSend(admin, copy, 0) != pdTRUE)
    Serial.println("#SIGNALV2,admin=BUSY");
  return true;
}
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
// Callback never reads Session. Withdrawal brackets all BLE-owner mutations;
// every early return publishes only a fully checked end-of-tick snapshot.
struct DisconnectPublication {
  DisconnectPublication() {
    portENTER_CRITICAL(&mux);
    disconnect_owner={};
    portEXIT_CRITICAL(&mux);
  }
  ~DisconnectPublication() {
    const uint32_t now=millis(), epoch=can_authorization_epoch.load();
    DisconnectOwner next;
    unsigned candidates=0;
    for(unsigned i=0;i<2;++i) {
      const auto l=snapshot(i);
      if(l.active && l.status && l.data && authorized(l) &&
         peers[i].generation==l.generation && peers[i].session.active(now) &&
         arm_owner.known && arm_owner.epoch==epoch &&
         arm_owner.handle==l.handle && arm_owner.generation==l.generation &&
         arm_owner.session==peers[i].session.id()) {
        ++candidates;
        next.known=true;next.handle=l.handle;next.generation=l.generation;
        next.epoch=epoch;next.deadline=peers[i].session.leaseDeadline();
      }
    }
    portENTER_CRITICAL(&mux);
    bool matching=false;
    for(const auto &l:links)
      if(l.active && l.status && l.data && l.handle==next.handle && l.generation==next.generation)
        matching=true;
    if(candidates==1 && matching && can_armed.load() &&
       can_authorization_epoch.load()==epoch) disconnect_owner=next;
    portEXIT_CRITICAL(&mux);
  }
};
#endif
void poll() {
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
  DisconnectPublication publication;
#endif
  if (!inbox)
    return;
  uint32_t now = millis();
  serviceControlRevocations(now);
#if defined(M5CAN_SIGNAL_V2_OWNER_AUTO_ARM)
  if(manual_stop_pending.exchange(false))manage("SIGV2 CAN STOP",now);
#endif
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
#if defined(M5CAN_SIGNAL_V2_OWNER_AUTO_ARM)
  serviceAutoArm(now);
#endif
  if(can_armed && int32_t(armed_until-now)<=0){
    owner.noteAuthorizationEnd("USB_ARM_EXPIRED");
    can_armed=false;const uint32_t revoked=++can_authorization_epoch;
    owner.revokeAuthorization(revoked);owner.invalidate(10);
#if defined(M5CAN_SIGNAL_V2_SERIAL_HIL)
    hil::note("kind=stop_request,epoch=%lu",(unsigned long)can_authorization_epoch.load());
#endif
    Serial.println("#SIGNALV2_CAN,armed=0,reason=USB_ARM_EXPIRED");
  }
  owner.tick(now,demand,can_armed,can_authorization_epoch.load());
  if(owner.stopped() && can_armed.exchange(false))++can_authorization_epoch;
#if defined(M5CAN_SIGNAL_V2_OWNER_CONTINUOUS_LIVE)
  display_reason.store(owner.stopReason());
  display_busy.store(owner.busy());
#endif
  static uint32_t stopped_ack_epoch=0;
  if(!can_armed && !owner.busy() && stopped_ack_epoch!=can_authorization_epoch.load()){
    stopped_ack_epoch=can_authorization_epoch.load();
#if defined(M5CAN_SIGNAL_V2_SERIAL_HIL)
    hil::note("kind=stopped,epoch=%lu,source_generation=%lu,mode_epoch=%lu",(unsigned long)stopped_ack_epoch,
      (unsigned long)owner.samples().source_generation,(unsigned long)owner.samples().mode_epoch);
#endif
    Serial.printf("#SIGNALV2_CAN,ack=STOPPED,epoch=%lu,source_generation=%lu,mode_epoch=%lu\n",
      static_cast<unsigned long>(stopped_ack_epoch),
      static_cast<unsigned long>(owner.samples().source_generation),
      static_cast<unsigned long>(owner.samples().mode_epoch));
  }
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
  // Single consumer: retain the FIFO head until its peer can accept Control.
  // The 12 bounded callback events include fragments. Other peers may wait
  // behind this head too; send/safety service above still runs on every tick.
  Incoming in;
  if (xQueuePeek(inbox, &in, 0) != pdTRUE)
    return;
  for (unsigned i = 0; i < 2; ++i) {
    auto l = snapshot(i);
    auto &p = peers[i];
    if (!l.active || l.handle != in.handle || l.generation != in.generation)
      continue;
    if (p.ack.size)
      return;
    xQueueReceive(inbox, &in, 0);
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
  // Disconnected/replaced generations must not block the live FIFO.
  xQueueReceive(inbox, &in, 0);
}
} // namespace signal_v2
#endif
