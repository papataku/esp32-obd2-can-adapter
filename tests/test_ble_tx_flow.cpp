#include <cassert>
#include <NimBLEDevice.h>
#include "ble_elm_transport.h"
#include "diagnostic_error_catalog.h"
using namespace m5can;
int main() {
  BleElmTransport ble;
  assert(ble.begin());
  auto* server=NimBLEDevice::createServer();
  ble_gap_conn_desc a{7},b{19};
  NimBLEServer::callbacks->onConnect(server,&a);
  NimBLEServer::callbacks->onConnect(server,&b);
  BleClientEvent ea{},eb{};
  assert(ble.pollClientEvent(ea) && ble.pollClientEvent(eb));
  uint8_t bytes[60];for(unsigned n=0;n<60;++n)bytes[n]=n;
  assert(ble.queueTo(7,ea.generation,bytes,60)==60);
  assert(ble.queueTo(19,eb.generation,bytes,60)==60);
  test_blocked_conn=7;test_notifications.clear();
  const auto start=millis();
  for(int n=0;n<3;++n)ble.pollTx();
  assert(millis()==start); // No synchronous sleeps for a blocked peer.
  assert(test_notifications.size()==3);
  for(const auto& item:test_notifications)assert(item.conn==19);
  assert(ble.stats().credit_waits==3 && ble.stats().notify_failures==0);
  test_blocked_conn=UINT16_MAX;
  for(int n=0;n<3;++n)ble.pollTx();
  assert(test_notifications.size()==6);
  unsigned offset=0;
  for(const auto& item:test_notifications)if(item.conn==7)
    for(auto byte:item.data)assert(byte==bytes[offset++]);
  assert(offset==60);
  // More than the old 100ms deadline must preserve bytes and other-peer service.
  test_busy_conn=7;test_notify_busy=200;
  assert(ble.queueTo(7,ea.generation,bytes,60)==60);
  test_notifications.clear();
  for(unsigned n=0;n<200;++n) {
    assert(ble.queueTo(19,eb.generation,bytes,1)==1);
    ble.pollTx();g_test_millis+=1;
  }
  assert(ble.stats().notify_failures==0 && test_disconnect_requests.empty());
  assert(test_notifications.size()==200);
  for(const auto& item:test_notifications)assert(item.conn==19);
  for(unsigned n=0;n<3;++n)ble.pollTx();
  offset=0;
  for(const auto& item:test_notifications)if(item.conn==7)
    for(auto byte:item.data)assert(byte==bytes[offset++]);
  assert(offset==60);
  test_busy_conn=UINT16_MAX;
  // Shared-pool reserve leaves output in RAM instead of allocating/retrying.
  test_msys_free=3;
  assert(ble.queueTo(7,ea.generation,bytes,1)==1);
  const auto before=ble.stats().targeted_notifications;
  ble.pollTx();assert(ble.stats().targeted_notifications==before);
  test_msys_free=5;ble.pollTx();
  assert(ble.stats().targeted_notifications==before);
  test_msys_free=6;ble.pollTx();
  test_msys_free=12;
  assert(ble.stats().targeted_notifications==before+1);
  // Exercise the same queue/controller predicate as the real NimBLE snapshot.
  test_host_queued=1;
  assert(ble.queueTo(7,ea.generation,bytes,1)==1);
  ble.pollTx(); // A small preloaded pipeline is allowed.
  test_host_queued=2;
  assert(ble.queueTo(7,ea.generation,bytes,1)==1);
  const auto queue_before=ble.stats().targeted_notifications;
  ble.pollTx();assert(ble.stats().targeted_notifications==queue_before);
  test_host_queued=0;test_host_controller=8;
  ble.pollTx();assert(ble.stats().targeted_notifications==queue_before);
  test_host_controller=0;ble.pollTx();
  assert(ble.stats().targeted_notifications==queue_before+1);
  // A competing host allocation can still win after the pool sample.
  test_alloc_failures=2;
  assert(ble.queueTo(7,ea.generation,bytes,1)==1);
  ble.pollTx();ble.pollTx();
  assert(ble.stats().mbuf_alloc_failures==2 && ble.stats().notify_failures==0);
  ble.pollTx();
  // Disconnect/reconnect must drop stale pending output, even on handle reuse.
  assert(ble.queueTo(7,ea.generation,bytes,60)==60);
  NimBLEServer::callbacks->onDisconnect(server,&a);
  NimBLEServer::callbacks->onConnect(server,&a);
  const auto fresh=ble.peerTraffic().peers[0].generation;
  assert(fresh!=ea.generation);
  assert(ble.queueTo(7,ea.generation,bytes,1)==0);
  test_notifications.clear();ble.pollTx();assert(test_notifications.empty());
  // Wrap the bounded FIFO while preserving exact response/prompt bytes.
  uint8_t wrap[3000];for(unsigned n=0;n<sizeof(wrap);++n)wrap[n]=n%251;
  for(unsigned run=0;run<3;++run) {
    assert(ble.queueTo(7,fresh,wrap,sizeof(wrap))==sizeof(wrap));
    assert(!ble.responseReady(7,fresh));
    test_notifications.clear();
    for(unsigned n=0;n<150;++n)ble.pollTx();
    offset=0;
    for(const auto& item:test_notifications)if(item.conn==7)
      for(auto byte:item.data)assert(byte==wrap[offset++]);
    assert(offset==sizeof(wrap));
    assert(ble.responseReady(7,fresh));
  }
  test_notifications.clear();
  // Capacity rejection is atomic and quarantines the session before async GAP.
  uint8_t full[4096]{};
  assert(ble.queueTo(7,fresh,full,sizeof(full))==sizeof(full));
  assert(ble.queueTo(7,fresh,bytes,1)==0);
  assert(ble.stats().tx_queue_overflows==1 && ble.stats().notify_failures==1);
  assert(test_disconnect_requests.size()==1 && test_disconnect_requests[0]==7);
  assert(ble.queueTo(7,fresh,bytes,1)==0);
  ble.pollTx();assert(test_notifications.empty());
  // A genuine stuck link has a per-peer progress deadline and one disconnect.
  test_blocked_conn=19;
  assert(ble.queueTo(19,eb.generation,bytes,1)==1);
  g_test_millis+=4999;ble.pollTx();assert(ble.stats().tx_stalls==0);
  g_test_millis+=1;ble.pollTx();assert(ble.stats().tx_stalls==1);
  assert(test_disconnect_requests.size()==2 && test_disconnect_requests[1]==19);
  ble.pollTx();assert(test_disconnect_requests.size()==2);
}
