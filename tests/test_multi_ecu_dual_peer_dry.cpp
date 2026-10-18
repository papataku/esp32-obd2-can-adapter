// End-to-end host simulation: real DualBleElm + ElmCompat + ISO-TP parser,
// fake serialized CAN owner, two BLE peers. No TWAI access or physical CAN TX.
#if !defined(M5CAN_MULTI_ECU_POLICY_DRY_BENCH) || !defined(M5CAN_CAN_MODE_DRY_RUN) || !defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
#error "This integration test requires dual BLE and physical CAN TX disabled"
#endif
#include <cassert>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <NimBLEDevice.h>
#include "dual_ble_elm.h"
#include "serial_protocol.h"
#include "drive_log.h"
#include "diagnostic_transaction.h"
using namespace m5can;

namespace {
enum class ReplyMode { Positive, Nrc02, ForeignOnly, Fault02 };
ReplyMode mode=ReplyMode::Positive;
bool hold=false, ready=false;
DiagnosticRequest queued{};
std::vector<DiagnosticRequest> requests;

TransactionFrame frame(uint32_t id,uint64_t t,
                       std::initializer_list<uint8_t> bytes) {
  TransactionFrame f{};
  f.can_id=id;
  f.extended=true;
  f.dlc=static_cast<uint8_t>(bytes.size());
  f.timestamp_us=t;
  size_t n=0;
  for(uint8_t b:bytes) f.data[n++]=b;
  return f;
}
DiagnosticResult replyFor(const DiagnosticRequest& request) {
  DiagnosticTransaction tx;
  const uint64_t t=static_cast<uint64_t>(millis())*1000U;
  tx.begin(request,t);
  const uint8_t source=static_cast<uint8_t>((request.can_id>>8)&0xFFU);
  const uint32_t response=0x18DAF100U|source;
  const uint32_t foreign=0x18DAF100U|(source==0x02?0x06:0x02);
  auto step=tx.onFrame(frame(foreign,t+10,
                          {0x10,0x0A,0x62,0x20,0x12,1,2,3}));
  assert(step.event==TransactionEvent::Ignored &&
         step.flow_control_can_id==0);
  if(mode==ReplyMode::ForeignOnly) {
    DiagnosticResult result=tx.result();
    result.status=DiagnosticStatus::NoData;
    return result;
  }
  if(mode==ReplyMode::Fault02 && source==0x02) {
    DiagnosticResult result=tx.result();
    result.status=DiagnosticStatus::BusOff;
    return result;
  }
  if(mode==ReplyMode::Nrc02 && source==0x02) {
    step=tx.onFrame(frame(response,t+20,{0x03,0x7F,0x22,0x31}));
    assert(step.event==TransactionEvent::Completed);
  } else {
    step=tx.onFrame(frame(response,t+20,
           {0x10,0x0A,0x62,0x20,0x12,source,0xAB,0xCD}));
    assert(step.event==TransactionEvent::NeedFlowControl);
    assert(step.flow_control_can_id==request.can_id);
    step=tx.onFrame(frame(foreign,t+25,
           {0x21,0xDE,0xAD,0xBE,0xEF}));
    assert(step.event==TransactionEvent::Ignored);
    step=tx.onFrame(frame(response,t+30,
           {0x21,0xDE,0xAD,0xBE,0xEF}));
    assert(step.event==TransactionEvent::Completed);
  }
  DiagnosticResult result=tx.result();
  result.status=DiagnosticStatus::Ok;
  return result;
}
std::string output(uint16_t conn) {
  std::string result;
  for(const auto& item:test_notifications) if(item.conn==conn)
    result.append(item.data.begin(),item.data.end());
  return result;
}
void write(ble_gap_conn_desc& p,const char* command) {
  NimBLECharacteristic c;
  NimBLECharacteristic::next_write_value=std::string(command)+"\r";
  NimBLECharacteristic::callbacks->onWrite(&c,&p);
}
struct Bench {
  CanMonitor can;
  BleElmTransport ble;
  SerialProtocol serial{can,&ble};
  DualBleElm dual{can,ble,serial};
  ble_gap_conn_desc a{7},b{19};
  Bench() {
    mode=ReplyMode::Positive;hold=false;ready=false;
    requests.clear();test_notifications.clear();g_test_millis=1000;
    assert(ble.begin());
    driveLog().begin(1);
    auto* server=NimBLEDevice::createServer();
    NimBLEServer::callbacks->onConnect(server,&a);
    NimBLEServer::callbacks->onConnect(server,&b);
    dual.poll();
    write(a,"ATE0");write(b,"ATE0");dual.poll();
    write(a,"ATH1");write(b,"ATH1");dual.poll();
    write(a,"ATS1");write(b,"ATS1");dual.poll();
    flush();
    test_notifications.clear();
  }
  void flush(){for(int i=0;i<128;i++)ble.pollTx();}
  void drain(){
    for(int i=0;i<50;i++){dual.poll();g_test_millis+=10;}
    flush();
  }
};
}
namespace m5can {
bool SerialProtocol::demoActive() const {return false;}
bool CanMonitor::acquireLease(uint32_t duration) {
  if(fault_locked_) return false;
  lease_deadline_ms_=millis()+duration;return true;
}
void CanMonitor::revokeLease(){lease_deadline_ms_=0;}
uint32_t CanMonitor::leaseRemainingMs() const {
  return lease_deadline_ms_>millis()?lease_deadline_ms_-millis():0;
}
CanStats CanMonitor::stats() const{return stats_;}
bool CanMonitor::submitQuery(const DiagnosticRequest& r) {
  assert(!query_in_flight_);
  assert(leaseRemainingMs()>0);
  assert(DiagnosticPolicy::allowedReadRequest(r));
  requests.push_back(r);queued=r;query_in_flight_=true;ready=true;
  return true;
}
bool CanMonitor::pollQueryResult(DiagnosticResult& r) {
  if(!ready||hold)return false;
  r=replyFor(queued);
  ready=false;query_in_flight_=false;
  if(r.status==DiagnosticStatus::BusOff) fault_locked_=true;
  return true;
}
}
int main() {
  // Different physical ECU, same DID, two independent BLE sessions.
  // No PID cache/peer join may mix results or collapse different CAN IDs.
  {Bench bench;
   write(bench.a,"ATM5R12:02:2012");
   write(bench.b,"ATM5R12:06:2012");
   bench.drain();
   assert(requests.size()==2);
   assert(requests[0].can_id==0x18DA02F1U);
   assert(requests[1].can_id==0x18DA06F1U);
   assert(output(7).find("18DAF102")!=std::string::npos);
   assert(output(19).find("18DAF106")!=std::string::npos);
   assert(output(7).find("18DAF106")==std::string::npos);
   assert(output(19).find("18DAF102")==std::string::npos);
   assert(output(7).find('>')!=std::string::npos);
   assert(output(19).find('>')!=std::string::npos);
  }
  // A nonmatching responder must never contaminate a successful other peer.
  {Bench bench;mode=ReplyMode::ForeignOnly;
   write(bench.a,"ATM5R12:02:2012");
   write(bench.b,"ATM5R12:06:2012");
   bench.drain();
   assert(requests.size()==2);
   assert(output(7).find("NO DATA")!=std::string::npos);
   assert(output(19).find("NO DATA")!=std::string::npos);
   assert(output(7).find("18DAF106")==std::string::npos);
   assert(output(19).find("18DAF102")==std::string::npos);
  }
  // One ECU's UDS rejection must not become the other ECU's response.
  {Bench bench;mode=ReplyMode::Nrc02;
   write(bench.a,"ATM5R12:02:2012");
   write(bench.b,"ATM5R12:06:2012");bench.drain();
   assert(requests.size()==2);
   assert(output(7).find("7F 22 31")!=std::string::npos);
   assert(output(19).find("18DAF106")!=std::string::npos);
   assert(output(19).find("7F 22 31")==std::string::npos);
  }
  // In-flight owner's connection handle is reused with a NEW generation.
  // Old ECU02 data must not be delivered to a new ECU06 request on handle 7.
  {Bench bench;hold=true;
   write(bench.a,"ATM5R12:02:2012");bench.dual.poll();
   assert(requests.size()==1);
   auto* server=NimBLEDevice::createServer();
   NimBLEServer::callbacks->onDisconnect(server,&bench.a);
   bench.dual.poll();
   NimBLEServer::callbacks->onConnect(server,&bench.a);
   bench.dual.poll();
   write(bench.a,"ATE0");bench.dual.poll();
   write(bench.a,"ATH1");bench.dual.poll();
   write(bench.a,"ATS1");bench.dual.poll();
   test_notifications.clear();
   write(bench.a,"ATM5R12:06:2012");bench.dual.poll();
   hold=false;bench.drain();
   assert(requests.size()==2);
   assert(requests[0].can_id==0x18DA02F1U);
   assert(requests[1].can_id==0x18DA06F1U);
   assert(output(7).find("18DAF102")==std::string::npos);
   assert(output(7).find("18DAF106")!=std::string::npos);
  }
  // If the in-flight owner disappears, surviving client still gets its own
  // request; completion from a dead owner must not be cached or misrouted.
  {Bench bench;hold=true;
   write(bench.a,"ATM5R12:02:2012");
   write(bench.b,"ATM5R12:06:2012");
   bench.dual.poll();assert(requests.size()==1);
   NimBLEServer::callbacks->onDisconnect(NimBLEDevice::createServer(),&bench.a);
   bench.dual.poll();hold=false;bench.drain();
   assert(requests.size()==2);
   assert(output(7).empty());
   assert(output(19).find("18DAF106")!=std::string::npos);
   assert(output(19).find("18DAF102")==std::string::npos);
  }
  // A transport fault is visible, never recycled as a fresh value.
  {Bench bench;mode=ReplyMode::Fault02;
   write(bench.a,"ATM5R12:02:2012");bench.drain();
   assert(output(7).find("BUS ERROR")!=std::string::npos);
   test_notifications.clear();
   write(bench.b,"ATM5R12:06:2012");bench.drain();
   assert(requests.size()==1);
   assert(output(19).find("M5CAN TX LOCKED")!=std::string::npos);
  }
  std::cout<<"PASS dual-peer physical UDS22 replay, ISO-TP provenance, reconnect and fault isolation\n";
  return 0;
}
