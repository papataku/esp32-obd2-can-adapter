#include <cassert>
#include <NimBLEDevice.h>
#include "ble_elm_transport.h"
#include "diagnostic_error_catalog.h"
#ifndef M5CAN_DUAL_NOTIFY_CHUNK_BYTES
#define M5CAN_DUAL_NOTIFY_CHUNK_BYTES 20
#endif
int main() {
  constexpr size_t limit=M5CAN_DUAL_NOTIFY_CHUNK_BYTES;
  m5can::BleElmTransport transport;
  assert(transport.begin());
  auto* server=NimBLEDevice::createServer();
  ble_gap_conn_desc a{7}, b{19};
  // NimBLE stores addresses little-endian: displayed 10:51:DB:8E:DD:40.
  const uint8_t aa[6]={0x40,0xdd,0x8e,0xdb,0x51,0x10};
  const uint8_t bb[6]={0x18,0xfb,0xe9,0xae,0x3d,0x98};
  for(size_t i=0;i<6;++i){a.peer_ota_addr.val[i]=aa[i];b.peer_ota_addr.val[i]=bb[i];}
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
  assert(transport.peerTraffic().peers[0].address_suffix==0xdd40);
  assert(transport.peerTraffic().peers[1].address_suffix==0xfb18);
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
  m5can::BleDiagnosticEvent diag;
  assert(transport.pollDiagnostic(diag) && diag.connection_id==7);
  assert(transport.pollDiagnostic(diag) && diag.connection_id==19);
  assert(transport.pollDiagnostic(diag));
  assert(diag.kind==m5can::BleDiagnosticEvent::Kind::Error);
  assert(diag.error_code==static_cast<uint16_t>(
      m5can::DiagnosticError::BleNotifyBackpressure));
  assert(diag.error_detail==3 && diag.generation==ea.generation);
  assert(!transport.pollDiagnostic(diag));
  ble_gap_event drop;
  drop.disconnect.conn=a; drop.disconnect.reason=0x208;
  assert(NimBLEDevice::gap_handler(&drop,nullptr)==0);
  assert(transport.pollDiagnostic(diag));
  assert(diag.kind==m5can::BleDiagnosticEvent::Kind::Disconnect);
  assert(diag.connection_id==7 && diag.generation==ea.generation);
  assert(diag.reason_known && diag.reason==0x208 && diag.mtu==247);
  assert(diag.retries==3 && diag.tx_bytes==sizeof(data)+21);
  // Logging a raw GAP observation cannot tear down a session itself.
  assert(transport.connectedClientCount()==2);
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
  bool saw_stale=false;
  while(transport.pollDiagnostic(diag)) {
    if(diag.error_code==static_cast<uint16_t>(
        m5can::DiagnosticError::BlePeerGenerationStale)) {
      assert(diag.generation==ea.generation);
      assert(diag.address_suffix==0); // never attribute to reused peer
      saw_stale=true;
    }
  }
  assert(saw_stale);
  // Diagnostic overflow stays bounded and never touches protocol events.
  for(unsigned n=0;n<70;++n) {
    drop.disconnect.reason=0x300+static_cast<int>(n);
    NimBLEDevice::gap_handler(&drop,nullptr);
  }
  assert(transport.diagnosticDrops()==7);
  unsigned logged=0;
  int last_reason=0;
  while(transport.pollDiagnostic(diag)) {
    ++logged;
    assert(diag.kind==m5can::BleDiagnosticEvent::Kind::Disconnect);
    last_reason=diag.reason;
  }
  // Never retain only the 63 oldest events while silently losing the
  // actual latest disconnection. Loss count remains explicit.
  assert(last_reason==0x300+69);
  assert(logged==63 && transport.connectedClientCount()==2);
  // If NimBLE's server callback fires without a corresponding GAP observer
  // notification, the journal must still report DISCONNECT reason_unknown.
  NimBLEServer::callbacks->onDisconnect(server,&b);
  assert(transport.pollDiagnostic(diag));
  assert(diag.kind==m5can::BleDiagnosticEvent::Kind::Disconnect);
  assert(diag.connection_id==19 && diag.generation==eb.generation);
  assert(!diag.reason_known && transport.connectedClientCount()==1);

}
