// Independent USB bench Central. No meter assets, vehicle pipeline or CAN
// driver.
#include "signal_v2_codec.h"
#include "signal_v2_golden.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <cstring>
using namespace signal_v2;
namespace {
NimBLEClient *client = nullptr;
NimBLERemoteCharacteristic *ctrl = nullptr;
QueueHandle_t received = nullptr;
struct Incoming {
  uint8_t lane = 0;
  uint16_t size = 0;
  uint8_t b[260]{};
};
Reassembler status_rx, data_rx;
Packet packet, last_request;
unsigned retries = 0, snapshot_attempts = 0;
uint32_t resync_started = 0;
uint32_t boot = 0, session = 0, rid = 0, revision = 0, seq = 0, transfer = 0,
         next = 0, waiting = 0;
bool hello = false, subscribed = false, resync = false;
uint32_t counts = 0, rejected = 0;
void receive(NimBLERemoteCharacteristic *c, uint8_t *b, size_t n, bool) {
  if (n > 260)
    return;
  Incoming in;
  in.lane = c->getUUID() == NimBLEUUID(Status) ? 3 : 2;
  in.size = n;
  std::copy(b, b + n, in.b);
  if (xQueueSend(received, &in, 0) != pdTRUE && client)
    client->disconnect();
}
bool sendLast();
bool request(uint8_t op, const uint8_t *body, size_t n) {
  uint8_t p[64]{};
  w32(p, ++rid);
  w32(p + 4, revision);
  p[8] = op;
  w16(p + 10, n);
  if (n)
    std::memcpy(p + 12, body, n);
  auto req =
      encode(1, op == 1 ? 0 : boot, op == 1 ? 0 : session, rid, p, 12 + n);
  last_request = req;
  retries = 0;
  return sendLast();
}
bool sendLast() {
  uint8_t f[260];
  ++transfer;
  status_rx.reset();
  for (size_t off = 0; off < last_request.size;) {
    auto z = fragment(last_request, 1, transfer, off, client->getMTU(), f);
    if (!ctrl->writeValue(f, z, true)) {
      client->disconnect();
      return false;
    }
    off += z - 16;
    delay(3);
  }
  waiting = millis() + 2000;
  return true;
}
void requireSnapshot(uint32_t now) {
  if (!resync) {
    resync_started = now;
    snapshot_attempts = 0;
  }
  resync = true;
  data_rx.reset();
}

void profile() {
  uint8_t b[32]{};
  w32(b, revision + 1);
  b[4] = 1;
  w16(b + 8, 1);
  b[10] = 1;
  w16(b + 12, 0x0c);
  b[14] = 1;
  b[15] = 0;
  w32(b + 16, 0x18daf101);
  w32(b + 20, 1);
  w16(b + 24, 200);
  w16(b + 26, 400);
  w16(b + 28, 600);
  request(2, b, 32);
}
} // namespace
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.printf("#RING_SIGNALV2,git=%s,target=SmartRing\n", M5CAN_BUILD_SHA);
  Serial.println("#RING_SIGNALV2,lab=1,can_driver=ABSENT,registration=USB_"
                 "APPROVAL_REQUIRED");
  received = xQueueCreate(16, sizeof(Incoming));
  NimBLEDevice::init("SmartRing-SignalV2-Lab");
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setSecurityAuth(true, false, true);
  ble_hs_cfg.store_status_cb = [](ble_store_status_event *, void *) {
    return BLE_HS_ESTORE_CAP;
  };
  client = NimBLEDevice::createClient();
  client->setConnectTimeout(5);
}
void loop() {
  if (Serial.available() && Serial.read() == 'g') {
    auto result = golden();
    for (unsigned i = 0; i < 12; ++i)
      Serial.printf("#RING_GOLDEN,G%02u=%s\n", i + 1,
                    result[i] ? "PASS" : "FAIL");
  }
  uint32_t now = millis();
  if (!client->isConnected()) {
    hello = subscribed = resync = false;
    boot = session = revision = seq = 0;
    waiting = 0;
    retries = snapshot_attempts = 0;
    resync_started = 0;
    status_rx.reset();
    data_rx.reset();
    xQueueReset(received);
    // An existing bond is never erased to "repair" authentication.
    auto scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    auto results = scan->start(3, false);
    for (int i = 0; i < results.getCount(); ++i) {
      auto d = results.getDevice(i);
      if (d.getAddress().toString() != "48:ca:43:3a:11:f6")
        continue;
      if (!client->connect(&d) || !client->secureConnection()) {
        client->disconnect();
        break;
      }
      auto service = client->getService(Service);
      if (!service) {
        client->disconnect();
        break;
      }
      ctrl = service->getCharacteristic(Control);
      auto st = service->getCharacteristic(Status);
      auto data = service->getCharacteristic(Data);
      if (!ctrl || !st || !data || !st->subscribe(false, receive) ||
          !data->subscribe(true, receive)) {
        client->disconnect();
        break;
      }
      Serial.printf("#RING_SIGNALV2,connected=1,mtu=%u,rssi=%d,awaiting_usb_"
                    "registration=1\n",
                    client->getMTU(), client->getRssi());
      next = millis();
      break;
    }
    scan->clearResults();
    delay(1000);
    return;
  }
  Incoming in;
  for (unsigned i = 0; i < 4 && xQueueReceive(received, &in, 0) == pdTRUE;
       ++i) {
    auto error = (in.lane == 3 ? status_rx : data_rx)
                     .feed(in.b, in.size, in.lane, now, packet);
    if (error == Error::Pending)
      continue;
    if (error != Error::None) {
      ++rejected;
      requireSnapshot(now);
      continue;
    }
    auto p = packet.payload();
    auto b = packet.bytes.data();
    if (in.lane == 3) {
      if (b[4] != 0x80 || u32(p) != rid)
        continue;
      waiting = 0;
      retries = 0;
      if (p[5] != 0) {
        Serial.printf("#RING_SIGNALV2,ack=%u,result=%u\n", p[4], p[5]);
        next = now + 1000;
        continue;
      }
      if (p[4] == 1) {
        boot = u32(b + 8);
        session = u32(b + 12);
        hello = true;
        Serial.printf("#RING_SIGNALV2,hello=ACCEPT,session=%lu\n",
                      (unsigned long)session);
        profile();
      } else if (u32(b + 8) == boot && u32(b + 12) == session) {
        revision = u32(p + 8);
        if (p[4] == 2) {
          subscribed = true;
          requireSnapshot(now);
          Serial.println("#RING_SIGNALV2,profile=ACCEPT,mode=DEMO");
        }
        next = now + 1000;
      }
    } else {
      if (!hello || !subscribed || u32(b + 8) != boot ||
          u32(b + 12) != session) {
        ++rejected;
        continue;
      }
      uint32_t frame = u32(b + 16);
      if (b[4] == 0x21) {
        if (!resync) {
          ++rejected;
          continue;
        }
        if (u32(p + 8) != revision || u32(p) != frame + 1) {
          ++rejected;
          continue;
        }
        bool valid = true;
        size_t offset = 16;
        for (unsigned j = 0; j < p[12]; ++j) {
          auto r = p + offset;
          valid &= r[3] == 2 && u32(r + 10) == revision && u32(r + 14) == 1 &&
                   u32(r + 18) == 1;
          offset += u16(r);
        }
        if (!valid) {
          ++rejected;
          continue;
        }
        resync = false;
        snapshot_attempts = 0;
        seq = frame;
      } else if (b[4] == 0x22) {
        seq = frame;
      } else if (b[4] == 0x20) {
        if (resync)
          continue;
        if (seq && frame != seq + 1) {
          requireSnapshot(now);
          ++rejected;
          continue;
        }
        seq = frame;
        const auto r = p + 2;
        if (r[3] != 2 || u32(r + 10) != revision || u32(r + 14) != 1 ||
            u32(r + 18) != 1) {
          ++rejected;
          continue;
        }
        ++counts;
        if (counts % 25 == 1)
          Serial.printf("#RING_SIGNALV2,data=%lu,origin=DEMO,rpm=0,rejected=%"
                        "lu,heap=%u\n",
                        (unsigned long)counts, (unsigned long)rejected,
                        ESP.getFreeHeap());
      }
    }
  }
  if (resync && resync_started && uint32_t(now - resync_started) >= 5000) {
    Serial.println("#RING_SIGNALV2,resync=INVALID_STOP");
    client->disconnect();
    return;
  }
  if (waiting && int32_t(now - waiting) >= 0) {
    if (++retries > 2) {
      Serial.println("#RING_SIGNALV2,ack=TIMEOUT");
      client->disconnect();
      return;
    }
    sendLast();
  }
  if (!waiting && int32_t(now - next) >= 0) {
    if (!hello) {
      uint8_t b[12] = {1, 2};
      w16(b + 4, 512);
      w16(b + 6, 244);
      request(1, b, 12);
    } else if (!subscribed)
      profile();
    else if (resync) {
      uint8_t b[8]{};
      w32(b, seq);
      w32(b + 4, revision);
      if (++snapshot_attempts > 2) {
        client->disconnect();
        return;
      }
      request(7, b, 8);
    } else
      request(4, nullptr, 0);
    next = now + 1000;
  }
  delay(2);
}
