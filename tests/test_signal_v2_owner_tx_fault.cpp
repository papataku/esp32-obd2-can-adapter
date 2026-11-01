// Actual monitor TX boundary: distinguish preflight STOP from a driver failure.
#include <array>
#include <atomic>
#include <string>
#include <vector>
#define private public
#include "can_monitor.h"
#undef private
#include <cassert>
#include <iostream>
static unsigned submissions=0;
esp_err_t twai_transmit(const twai_message_t*,TickType_t){++submissions;return ESP_FAIL;}
esp_err_t twai_get_status_info(twai_status_info_t*){return ESP_OK;}
esp_err_t twai_receive(twai_message_t*,TickType_t){return ESP_FAIL;}
int main(){
  m5can::CanMonitor m;
  twai_message_t frame{};frame.extd=1;frame.identifier=0x18DA01F1;
  assert(m.transmitOwnedFrame(frame,20)!=ESP_OK);
  assert(submissions==0&&!m.faultLocked()); // STOP/expired lease isn't a TX fault
  assert(m.acquireLease(550));
  assert(m.transmitOwnedFrame(frame,20)!=ESP_OK);
  assert(submissions==1&&m.faultLocked()&&m.leaseRemainingMs()==0);
  assert(!m.acquireLease(550)); // reconnect/USB cannot revive the driver capability
  assert(m.transmitOwnedFrame(frame,20)!=ESP_OK&&submissions==1);
  std::cout<<"PASS owner-live real TX failure boot latch; preflight STOP exempt\n";
}
