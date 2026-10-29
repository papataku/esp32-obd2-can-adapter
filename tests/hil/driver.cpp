#include <cassert>
#include <iostream>
#include <thread>
#include <atomic>
#define private public
#include "can_monitor.h"
#undef private
#include "twai_hil.h"
uint32_t g_test_millis=0;
SerialStub Serial;
static std::atomic<bool> pauseAdmission{false},parsed{false},resumeAdmission{false};
namespace hil {void admissionHook(){
  if(!pauseAdmission.load())return;
  parsed=true;while(!resumeAdmission.load())std::this_thread::yield();
}}
static void inject(const char* body,uint32_t gen=0){
  const std::string cmd="HIL RX "+std::to_string(gen?gen:hil::currentGeneration())+" "+body;
  hil::usbCommand(cmd.c_str());
}
static void flush(){for(int i=0;i<1000;++i)hil::poll();}
int main(int argc,char** argv){
  const std::string test=argc>1?argv[1]:"drain";
  twai_general_config_t g{};g.mode=TWAI_MODE_NORMAL;g.rx_queue_len=256;
  twai_timing_config_t t{};twai_filter_config_t f{};twai_status_info_t s{};twai_message_t m{};
  assert(hil::driver_install(&g,&t,&f)==ESP_OK);assert(hil::start()==ESP_OK);flush();
  if(test=="drain"){
    inject("18DAF101 1 06410C0FA00D00");
    assert(hil::stop()==ESP_OK);auto before=g_test_millis;
    assert(hil::receive(&m,0)==ESP_OK);assert(g_test_millis==before);
    assert(m.identifier==0x18DAF101);assert(hil::receive(&m,0)==ESP_ERR_TIMEOUT);
    assert(g_test_millis==before);
    // Real production switchDriverMode drains the shim while stopped and
    // captures a late frame through the active probe guard before reinstall.
    assert(hil::start()==ESP_OK);inject("18DAF101 1 06410C0FA00D00");
    m5can::CanMonitor monitor;Q frames{256,sizeof(m5can::CapturedFrame),{}};monitor.frame_queue_=&frames;monitor.probe_guard_=true;
    monitor.probe_tx_us_=1;g_test_millis=200;
    assert(monitor.switchDriverMode(TWAI_MODE_LISTEN_ONLY));
    assert(monitor.faultLocked());assert(monitor.stats().rx_frames==1);
  }else if(test=="model"){
    for(int i=0;i<256;++i){inject("18DAF101 1 00");flush();}
    hil::get_status_info(&s);assert(s.msgs_to_rx==256 && s.rx_missed_count==0);
    inject("18DAF101 1 00");hil::get_status_info(&s);assert(s.rx_missed_count==1);
    hil::stop();inject("18DAF101 1 00");hil::get_status_info(&s);
    assert(s.state==TWAI_STATE_STOPPED && s.rx_missed_count==1);
    hil::driver_uninstall();hil::driver_install(&g,&t,&f);hil::start();
    hil::get_status_info(&s);assert(!s.rx_missed_count && !s.msgs_to_rx);
    hil::usbCommand("HIL BUSOFF");assert(hil::transmit(&m,20)==ESP_ERR_INVALID_STATE);
    inject("18DAF101 1 00");hil::get_status_info(&s);
    assert(s.state==TWAI_STATE_BUS_OFF && !s.msgs_to_rx && !s.rx_missed_count);
  }else if(test=="race"){
    // Force injection to wait at the exact admission boundary while the owner
    // performs stop/drain. All state and storage accesses use the same mutex.
    const auto old=hil::currentGeneration();pauseAdmission=true;
    std::thread usb([&]{inject("18DAF101 1 00",old);});
    while(!parsed.load())std::this_thread::yield();
    assert(hil::stop()==ESP_OK);
    assert(hil::receive(&m,0)==ESP_ERR_TIMEOUT);
    hil::driver_uninstall();hil::driver_install(&g,&t,&f);hil::start();
    resumeAdmission=true;usb.join();pauseAdmission=false;
    assert(hil::receive(&m,0)==ESP_ERR_TIMEOUT);hil::get_status_info(&s);
    assert(!s.rx_missed_count); // no old injection can cross the generation
    for(int i=0;i<100;++i){
      inject("18DAF101 1 00");hil::stop();
      assert(hil::receive(&m,0)==ESP_OK);assert(hil::receive(&m,0)==ESP_ERR_TIMEOUT);
      hil::driver_uninstall();hil::driver_install(&g,&t,&f);hil::start();flush();
    }
  }else if(test=="transport"){
    // USB is never serviced: owner operations remain bounded, missing evidence
    // latches fail-closed and cannot be reset away by reinstall or HIL RESET.
    for(int i=0;i<128;++i)hil::transmit(&m,20);
    assert(hil::transmit(&m,20)!=ESP_OK);hil::get_status_info(&s);assert(s.rx_missed_count);
    hil::stop();hil::driver_uninstall();hil::driver_install(&g,&t,&f);
    assert(hil::start()!=ESP_OK);Serial.lines.clear();flush();
    assert(Serial.lines.find("lost=0,")==std::string::npos);
  }
  std::cout<<"PASS HIL "<<test<<"\n";
}
