#include <cassert>
#include <NimBLEDevice.h>
#include "ble_elm_transport.h"
#ifndef M5CAN_DUAL_NOTIFY_CHUNK_BYTES
#define M5CAN_DUAL_NOTIFY_CHUNK_BYTES 20
#endif
int main() {
  constexpr size_t limit=M5CAN_DUAL_NOTIFY_CHUNK_BYTES;
  m5can::BleElmTransport transport;
  assert(transport.begin());
  auto* server=NimBLEDevice::createServer();
  ble_gap_conn_desc a{7}, b{19};
  NimBLEServer::callbacks->onConnect(server,&a);
  NimBLEServer::callbacks->onConnect(server,&b);
  NimBLEServer::callbacks->onMTUChange(247,&a);
  NimBLEServer::callbacks->onMTUChange(255,&b);
  m5can::BleClientEvent ea{},eb{};
  assert(transport.pollClientEvent(ea));
  assert(transport.pollClientEvent(eb));
  assert(transport.connectedClientCount()==2);
  assert(transport.peerTraffic().peers[0].connected);
  assert(transport.peerTraffic().peers[1].connected);
  assert(transport.peerTraffic().peers[0].connection_id==7);
  assert(transport.peerTraffic().peers[1].connection_id==19);
  NimBLECharacteristic characteristic;
  NimBLECharacteristic::next_write_value="ATM5B00:010C\r";
  NimBLECharacteristic::callbacks->onWrite(&characteristic,&a);
  auto peers=transport.peerTraffic();
  assert(peers.peers[0].rx_writes==1);
  assert(peers.peers[0].rx_bytes==NimBLECharacteristic::next_write_value.size());
  assert(peers.peers[1].rx_bytes==0);

  uint8_t data[203]{};
  for(size_t i=0;i<sizeof(data);++i)data[i]=static_cast<uint8_t>(i);

  assert(transport.writeTo(7,ea.generation,data,sizeof(data))==sizeof(data));
  assert(transport.peerTraffic().peers[0].tx_bytes==sizeof(data));
  assert(transport.peerTraffic().peers[1].tx_bytes==0);

  assert(test_notifications.size()==(sizeof(data)+limit-1)/limit);
  size_t offset=0;
  for(const auto& n:test_notifications) {
    assert(n.conn==7);
    for(size_t i=0;i<n.length;++i)assert(n.data[i]==data[offset+i]);
    offset+=n.length;
  }
  assert(offset==sizeof(data));
  test_notifications.clear();
  test_notify_busy=3;
  assert(transport.writeTo(7,ea.generation,data,21)==21);
  assert(test_notifications.size()==(21+limit-1)/limit);
  size_t retry_offset=0;
  for(const auto& n:test_notifications) {
    for(size_t i=0;i<n.length;++i)assert(n.data[i]==data[retry_offset+i]);
    retry_offset+=n.length;
  }
  assert(retry_offset==21);
  assert(transport.stats().notify_retries==3);
  assert(transport.peerTraffic().peers[0].notify_retries==3);
  assert(transport.peerTraffic().peers[0].tx_bytes==sizeof(data)+21);

  assert(transport.stats().notify_failures==0);
  for(const auto& n:test_notifications) {assert(n.conn==7); assert(n.length<=limit);}
  test_notifications.clear();
  NimBLEServer::callbacks->onDisconnect(server,&a);
  assert(transport.connectedClientCount()==1);
  assert(!transport.peerTraffic().peers[0].connected);
  assert(transport.peerTraffic().peers[1].connected);

  assert(transport.writeTo(7,ea.generation,data,sizeof(data))==0);
  assert(transport.writeTo(19,eb.generation,data,21)==21);
  assert(test_notifications.size()==(21+limit-1)/limit);
  for(const auto& n:test_notifications) {assert(n.conn==19);assert(n.length<=limit);}
  // Even the matched profile must honor a peer that negotiates MTU23.
  NimBLEServer::callbacks->onMTUChange(23,&b);
  test_notifications.clear();
  assert(transport.writeTo(19,eb.generation,data,sizeof(data))==sizeof(data));
  assert(test_notifications.size()==11);
  for(const auto& n:test_notifications) {assert(n.conn==19);assert(n.length<=20);}
  NimBLEServer::callbacks->onConnect(server,&a);
  const auto after_reconnect=transport.peerTraffic();
  assert(after_reconnect.peers[0].connected);
  assert(after_reconnect.peers[0].generation!=ea.generation);
  assert(after_reconnect.peers[0].rx_bytes==0);
  assert(after_reconnect.peers[0].tx_bytes==0);
  assert(transport.writeTo(7,ea.generation,data,1)==0);
}
