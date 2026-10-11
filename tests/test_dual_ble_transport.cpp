#include <cassert>
#include <NimBLEDevice.h>
#include "ble_elm_transport.h"
int main() {
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
  uint8_t data[203]{};
  assert(transport.writeTo(7,ea.generation,data,sizeof(data))==sizeof(data));
  assert(test_notifications.size()==11);
  test_notifications.clear();
  test_notify_busy=3;
  assert(transport.writeTo(7,ea.generation,data,21)==21);
  assert(test_notifications.size()==2);
  assert(transport.stats().notify_retries==3);
  assert(transport.stats().notify_failures==0);
  for(const auto& n:test_notifications) {assert(n.conn==7); assert(n.length<=20);}
  test_notifications.clear();
  NimBLEServer::callbacks->onDisconnect(server,&a);
  assert(transport.connectedClientCount()==1);
  assert(transport.writeTo(7,ea.generation,data,sizeof(data))==0);
  assert(transport.writeTo(19,eb.generation,data,21)==21);
  assert(test_notifications.size()==2);
  for(const auto& n:test_notifications) {assert(n.conn==19);assert(n.length<=20);}
  NimBLEServer::callbacks->onConnect(server,&a);
  assert(transport.writeTo(7,ea.generation,data,1)==0);
}
