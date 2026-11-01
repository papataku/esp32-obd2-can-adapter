// Deterministic CAN-less serial-HIL: production owner, monitor, budgets, codec,
// transaction and serial TWAI shim. Only shim IO schedules a synthetic ECU.
#include <array>
#include <atomic>
#include <string>
#include <vector>
#include <iostream>
#include <cassert>
#include <Arduino.h>
#define private public
#include "can_monitor.h"
#undef private
#include "signal_v2_can_owner.h"
SerialStub Serial;
uint32_t g_test_millis=1000;
namespace hil {
void admissionHook(){}
esp_err_t shimTransmit(const twai_message_t*,TickType_t);
esp_err_t shimReceive(twai_message_t*,TickType_t);
static void inject(const char* bytes) {
  const std::string cmd="HIL RX "+std::to_string(currentGeneration())+" 18DAF101 1 "+bytes;
  assert(usbCommand(cmd.c_str()));
}
esp_err_t transmit(const twai_message_t* f,TickType_t ticks) {
  const auto result=shimTransmit(f,ticks);
  if(result==ESP_OK) {
    if(f->data[0]==3)inject("06410C13880D00");
    else if(f->data[0]==6)inject("1011410D000C1388");
    else if(f->data[0]==0x30){inject("2105805B7F9A0000");inject("224B000064000000");}
  }
  return result;
}
esp_err_t receive(twai_message_t* f,TickType_t ticks){return shimReceive(f,ticks);}
}
int main() {
  SerialStub::capture=true;
  m5can::CanMonitor monitor;assert(monitor.begin());
  signal_v2::CanOwner owner;owner.attach(&monitor);
  #if defined(M5CAN_SIGNAL_V2_10HZ_LAB)
  const signal_v2::Simulator::Demand demand{{100,100,100,100,100}};
#else
  const signal_v2::Simulator::Demand demand{{200,200,200,200,200}};
#endif
  while(g_test_millis<3800) {
    owner.tick(g_test_millis,demand,true,1);
    m5can::DiagnosticRequest q{};
    if(xQueueReceive(monitor.query_queue_,&q,0)==pdTRUE) {
      std::cout<<"REQUEST t="<<g_test_millis<<" id="<<q.can_id<<" epoch="<<q.authorization_epoch
        <<" deadline="<<q.timeout_ms<<" bytes=";
      for(unsigned i=0;i<q.length;++i)std::cout<<unsigned(q.data[i])<<',';
      std::cout<<'\n';
      m5can::DiagnosticResult r{};monitor.performQuery(q,r);
      assert(r.status==m5can::DiagnosticStatus::Ok);
      assert(xQueueSend(monitor.result_queue_,&r,0)==pdTRUE);
    }
    for(unsigned i=0;i<100;++i)hil::poll();
    ++g_test_millis;
  }
  assert(owner.samples().acquired>=10);
  monitor.revokeAuthorization(2);owner.tick(g_test_millis,demand,false,2);
  const auto before=monitor.stats().tx_attempts;
  for(unsigned i=0;i<1000;++i)owner.tick(++g_test_millis,demand,false,2);
  assert(before==monitor.stats().tx_attempts);
  std::cout<<"STOP tx="<<before<<" acquired="<<owner.samples().acquired<<'\n';
  std::cout<<SerialStub::lines;
}
