// Exercise the actual lab USB command parser without opening a serial port.
#include "../src/discovery11_lab.cpp"
#include <cassert>
#include <iostream>
SerialStub Serial;
uint32_t g_test_millis=1;
namespace hil {void admissionHook(){}}
namespace signal_v2 {
struct CanOwnerTestAccess {
  static void service(m5can::CanMonitor& m) {
    m5can::DiagnosticRequest q{};
    if(xQueueReceive(m.query_queue_,&q,0)==pdTRUE){
      m5can::DiagnosticResult r{};m.performQuery(q,r);
      assert(xQueueSend(m.result_queue_,&r,0)==pdTRUE);
    }
  }
};
}
static bool injected=false;
void p5TimeHook() {
  hil::poll();
  if(!injected && monitor.stats().tx_success) {
    injected=true;
    const auto rx="HIL RX "+std::to_string(hil::currentGeneration())+" 7E8 0 0641001234567800";
    command(rx.c_str());
    command("P5 PROFILE CHANGE"); // Real USB STOP while owner task waits for RX.
  }
}
int main() {
  SerialStub::capture=true;setup();
  assert(owner.stopped() && !armed && monitor.stats().tx_attempts==0);
  command("ARM");command("SIGV2 CAN S1");loop();
  assert(!armed && monitor.stats().tx_attempts==0);
  command("P5 ARM");assert(armed);const auto before=epoch;
  command("P5 ARM");assert(epoch==before); // explicit duplicate cannot renew ARM
  loop();signal_v2::CanOwnerTestAccess::service(monitor);loop();
  assert(owner.stopped() && !armed && injected);
  assert(monitor.stats().tx_success==1 && monitor.stats().signal_v2_fc==0);
  assert(Serial.lines.find("Learning=1")==std::string::npos);
  command("P5 STOP");loop();assert(monitor.stats().tx_attempts==1);
  assert(owner.samples().acquired==0);
  std::cout<<"PASS lab USB parser: boot/no auto ARM, duplicate ARM, profile race, STOP; TX1 FC0 LIVE0\n";
}
