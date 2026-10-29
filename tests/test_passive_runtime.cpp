// Exercise the actual target application against fake SDK registers/driver.
#include <passive_test_sdk.h>
#include "../src/passive_rx.cpp"
#include <algorithm>
int main(int argc,char** argv) {
  assert(argc==2);
  for (unsigned i=0;i<49;++i) GPIO_PIN_MUX_REG[i]=i;
  const std::string fault=argv[1];
  install_fail=fault=="install"; start_fail=fault=="start"; task_fail=fault=="task";
  setup();
  if (install_fail || start_fail || task_fail) {
    assert(stats.fault && !stats.verified && !installed);
    if (!install_fail) assert(calls.back()=="uninstall");
    return 0;
  }
  assert(fresh(readStats()) && calls.back()=="task");
  Serial.input="SIGV2 CAN ARM\nARM\nLEASE 1000\nATM5TX1\nSTATS\n";
  loop();
  assert(Serial.output.find("#REJECT,PASSIVE_IMMUTABLE")!=std::string::npos);
  assert(Serial.output.find("self_check=OK")!=std::string::npos);
  assert(calls.size()==3); // No command reached the driver.
  receive_result=ESP_OK; receive_extended=false; receiveOnce();
  receive_extended=true; receiveOnce();
  assert(stats.frames==2 && stats.standard==1 && stats.extended==1);
  receive_result=ESP_ERR_TIMEOUT; receiveOnce(); assert(stats.frames==2);
  fake_info.bus_error_count=7; fake_info.arb_lost_count=9;
  fake_ms=100; assert(check()); assert(stats.bus_errors==7 && stats.arb_lost==9);
  fake_ms=200;
  if (fault.rfind("route",0)==0) GPIO.func_out_sel_cfg[std::stoi(fault.substr(5))].func_sel=TWAI_TX_IDX;
  else if (fault=="output") GPIO.out=0;
  else if (fault=="pad") pad_high=false;
  else if (fault=="enable") GPIO.enable=0;
  else if (fault=="inversion") GPIO.func_out_sel_cfg[13].inv_sel=1;
  else if (fault=="oen") GPIO.func_out_sel_cfg[13].oen_sel=0;
  else if (fault=="mux") registers[13]=0;
  else if (fault=="input") registers[13]&=~FUN_IE;
  else if (fault=="sleep") registers[13]|=SLP_SEL;
  else if (fault=="rtc") registers[1000]|=RTC_IO_TOUCH_PAD13_MUX_SEL;
  else if (fault=="open_drain") GPIO.pin[13].pad_driver=true;
  else if (fault=="rx") GPIO.func_in_sel_cfg[TWAI_RX_IDX].val^=1;
  else if (fault=="mode") TWAI.mode_reg.lom=false;
  else if (fault=="self_test") TWAI.mode_reg.stm=true;
  else if (fault=="state") fake_info.state=TWAI_STATE_STOPPED;
  else if (fault=="status") status_fail=true;
  else if (fault=="late") fake_ms=301;
  else assert(false);
  assert(!check());
  assert(stats.fault && !stats.verified && !installed);
  assert(calls[calls.size()-2]=="stop" && calls.back()=="uninstall");
  for (auto& r:GPIO.func_out_sel_cfg) assert(r.func_sel!=TWAI_TX_IDX);
  assert(GPIO.out&(1U<<13));
  const auto n=calls.size();
  assert(!check()); assert(calls.size()==n); // No restart, no second uninstall.
  loop();
  assert(Serial.output.find("#PASSIVE_FAULT,CAN_OFF_LATCHED")!=std::string::npos);
}
