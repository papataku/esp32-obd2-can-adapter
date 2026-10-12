#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <Preferences.h>
#include "ble_evidence_journal.h"

using namespace m5can;

namespace {
uint8_t active_peers=0;
BleDiagnosticEvent sample(uint32_t seq,BleDiagnosticEvent::Kind kind) {
  BleDiagnosticEvent e{};
  e.kind=kind;e.sequence=seq;e.uptime_ms=g_test_millis;
  e.connection_id=7;e.address_suffix=0xDD41;e.generation=3;
  e.mtu=247;e.interval=12;e.latency=0;e.supervision=300;
  e.reason_known=kind==BleDiagnosticEvent::Kind::Disconnect;
  e.reason=kind==BleDiagnosticEvent::Kind::Disconnect?520:0;
  e.rx_bytes=120000;e.tx_bytes=92000;e.failed=4;e.retries=19;
  return e;
}
} // namespace

namespace m5can {
uint8_t BleElmTransport::connectedClientCount() const {return active_peers;}
}

int main() {
  Preferences::storage.clear();Preferences::writes=0;
  CanMonitor can;
  BleElmTransport transport;
  BleEvidenceJournal first;
  g_test_millis=1000;first.begin(41,"known-commit");
  const auto connect=sample(1,BleDiagnosticEvent::Kind::Connect);
  first.append(connect,0);
  assert(first.snapshot().count==1 && first.snapshot().total==1);
  // Existing CAN owner lock prevents a competing NVS write.
  assert(can.tryBeginLogCheckpoint());
  assert(!first.poll(can) && Preferences::writes==0);
  can.endLogCheckpoint();
  assert(first.poll(can) && first.saved() && Preferences::writes==1);

  g_test_millis=2000;
  first.append(sample(3,BleDiagnosticEvent::Kind::Disconnect),2);
  assert(first.snapshot().seq_gaps==1);
  assert(first.snapshot().queue_drops==2);
  assert(!first.poll(can)); // write cadence; do not wear NVS on flapping
  g_test_millis=6500;
  assert(first.poll(can) && first.saved());
  assert(Preferences::writes==2);
  for(unsigned i=4;i<=37;++i) {
    ++g_test_millis;
    first.append(sample(i,BleDiagnosticEvent::Kind::Connect),2);
    assert(!first.poll(can));
  }
  assert(first.snapshot().total==36 && first.snapshot().evicted==12);
  assert(first.snapshot().count==BleEvidenceJournal::kEvents);
  assert(first.snapshot().events[0].seq==14);
  assert(Preferences::writes==2);
  g_test_millis+=31000;
  assert(first.poll(can));
  assert(Preferences::writes==3);

  BleEvidenceJournal after_reboot;
  after_reboot.begin(42,"next-build");
  SerialStub::capture=true;SerialStub::lines.clear();
  after_reboot.dump(can,transport,41);
  const std::string exported=SerialStub::lines;
  assert(exported.find("#BLELOG,BOOT,boot=41,state=persisted")!=std::string::npos);
  assert(exported.find("queue_drops=2")!=std::string::npos);
  assert(exported.find("seq_gaps=1")!=std::string::npos);
  assert(exported.find("total=36,kept=24,evicted=12")!=std::string::npos);
  assert(exported.find("seq=37")!=std::string::npos);
  // Inspect a disconnect BEFORE the 24-item ring has evicted it.
  BleEvidenceJournal disconnect_probe;
  disconnect_probe.begin(43,"test-2");
  g_test_millis+=1000;
  disconnect_probe.append(sample(1,BleDiagnosticEvent::Kind::Disconnect),0);
  disconnect_probe.poll(can);
  g_test_millis+=7000;
  disconnect_probe.poll(can);
  SerialStub::lines.clear();
  after_reboot.dump(can,transport,43);
  assert(SerialStub::lines.find("reason_raw=520")!=std::string::npos);
  assert(SerialStub::lines.find("gen=3,peer_suffix=DD41")!=std::string::npos);

  // Active BLE/CAN reads must never be blocked by bulk serial exports.
  SerialStub::lines.clear();active_peers=1;
  after_reboot.dump(can,transport,41);
  assert(SerialStub::lines.find("ACTIVE_TRAFFIC")!=std::string::npos);
  active_peers=0;
  // CRC check must distinguish corruption from an apparently valid boot.
  auto& bytes=Preferences::storage["m5can_blelog/ble1"];
  assert(!bytes.empty());
  bytes[bytes.size()-1]^=0x40;
  SerialStub::lines.clear();
  after_reboot.dump(can,transport,41);
  assert(SerialStub::lines.find("#BLELOG,MISSING,boot=41")!=std::string::npos);
  SerialStub::capture=false;SerialStub::lines.clear();

  // Cross-domain faults are independent of the transient last-24-event
  // ring. CAN and BLE counters contribute their actual DELTA, not one
  // count for each periodic sampling cycle.
  BleEvidenceJournal faults;
  g_test_millis+=1200;
  faults.begin(44,"fault-matrix");
  CanStats can_stat{};
  BleStats ble_stat{};
  can_stat.query_timeout=4;
  can_stat.query_failed=3;
  can_stat.driver_rx_overrun=2;
  can_stat.driver_rx_missed=8;
  can_stat.driver_bus_error=17;
  can_stat.driver_arb_lost=9;
  can_stat.app_queue_drops=11;
  ble_stat.rx_overflow=6;
  ble_stat.notify_failures=2;
  ble_stat.notify_retries=50;
  ble_stat.session_rejections=1;
  faults.observeHealth(can_stat,ble_stat);
  faults.observeHealth(can_stat,ble_stat); // no double counting
  can_stat.driver_bus_error+=3;
  faults.observeHealth(can_stat,ble_stat);
  faults.appendError(DiagnosticError::CanIsoTpError,22);
  faults.appendError(DiagnosticError::CanBusOff,3);
  faults.appendError(DiagnosticError::GroupRejected,5);
  faults.appendError(DiagnosticError::ElmInputOverflow,129);
  faults.appendError(DiagnosticError::JournalStorageFailure,1);
  assert(faults.poll(can));
  BleEvidenceJournal persisted_faults;
  persisted_faults.begin(45,"after-fault");
  SerialStub::capture=true;SerialStub::lines.clear();
  persisted_faults.dump(can,transport,44);
  const auto fault_dump=SerialStub::lines;
  assert(fault_dump.find("name=CAN_TIMEOUT_COUNTER,count=4")!=std::string::npos);
  assert(fault_dump.find("name=CAN_BUS_ERRORS,count=20")!=std::string::npos);
  assert(fault_dump.find("name=BLE_TX_BACKPRESSURE,count=50")!=std::string::npos);
  assert(fault_dump.find("name=ISOTP_FAILURE,count=1")!=std::string::npos);
  assert(fault_dump.find("name=PID_GROUP_REJECTED,count=1")!=std::string::npos);
  assert(fault_dump.find("name=ELM_INPUT_OVERFLOW,count=1")!=std::string::npos);
  assert(fault_dump.find("name=JOURNAL_NVS_FAILURE,count=1")!=std::string::npos);
  assert(fault_dump.find("first_detail=17,last_detail=3")!=std::string::npos);
  SerialStub::capture=false;SerialStub::lines.clear();

  // Journal and CAN trip history occupy separate NVS namespaces. The
  // legacy trip key, if present, remains byte-for-byte untouched.
  assert(Preferences::storage.count("m5can_trip/trip1")==0);
  std::cout<<"PASS persisted BLE evidence, disconnect reason, corruption, "
              "overrun, CAN-idle lock, write cadence and reboot export\n";
  return 0;
}
