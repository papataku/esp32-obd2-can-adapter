// Exercise the actual target application against fake SDK registers/driver.
#include <passive_test_sdk.h>
#include "../src/passive_rx.cpp"
#include <algorithm>
int main(int argc,char** argv) {
  assert(argc==2);
  for (unsigned i=0;i<49;++i) GPIO_PIN_MUX_REG[i]=i;
  const std::string fault=argv[1];
  lgfx::display_ok=fault!="display";
  install_fail=fault=="install"; start_fail=fault=="start"; task_fail=fault=="task";
  if (fault=="before-pad") stuck_pad_low=true;
  if (fault=="display-clobber") display_disables_gpio13=true;
  if (fault=="display-clobber-stuck") {
    display_disables_gpio13=true; display_clobber_external_low=true;
  }
  if (fault=="display-low") display_drives_gpio13_low=true;
  if (fault=="display-route") display_corrupts_gpio13_route=true;
  if (fault=="before-rtc") stuck_rtc_mux=true;
  if (fault=="before-sleep") stuck_sleep=true;
  if (fault=="before-mux") bad_iomux=true;
  if (fault=="before-input") bad_input=true;
  if (fault=="stopped-rx") bad_rx_on_install=true;
  if (fault=="start-nolom") skip_listen_on_start=true;
  if (fault=="status-boot") status_fail=true;
  setup();
  const bool early=(fault=="display" || fault=="install" || fault=="start" ||
      fault=="task" || fault=="before-pad" || fault=="before-rtc" ||
      fault=="before-sleep" || fault=="before-mux" || fault=="before-input" ||
      fault=="stopped-rx" || fault=="start-nolom" || fault=="status-boot" ||
      fault=="display-low" || fault=="display-route" ||
      fault=="display-clobber-stuck");
  if (early) {
    assert(stats.fault && !stats.verified && !installed);
    assert(fault_evidence.captured);
    std::string stage,field;
    if (fault=="display") { stage="display_init"; field="display.init"; }
    else if (fault=="install") { stage="driver_install"; field="twai_driver_install"; }
    else if (fault=="start") { stage="driver_start"; field="twai_start"; }
    else if (fault=="task") { stage="task_create"; field="xTaskCreatePinnedToCore"; }
    else if (fault=="stopped-rx") { stage="stopped_snapshot"; field="rx_route"; }
    else if (fault=="start-nolom") { stage="initial_check"; field="controller_listen_only"; }
    else if (fault=="status-boot") { stage="initial_check"; field="twai_status"; }
    else if (fault=="display-low") { stage="post_display_unsafe"; field="txd_latch_high"; }
    else if (fault=="display-route") { stage="post_display_unsafe"; field="txd_gpio"; }
    else if (fault=="display-clobber-stuck") { stage="post_display_unsafe"; field="txd_gpio"; }
    else {
      stage="first_hold";
      if (fault=="before-pad") field="txd_pad_high";
      if (fault=="before-rtc" || fault=="before-sleep") field="txd_awake";
      if (fault=="before-mux") field="txd_mux_gpio";
      if (fault=="before-input") field="txd_input_enabled";
    }
    assert(stage==fault_evidence.stage && field==fault_evidence.field);
    if (fault=="before-pad") assert(!fault_evidence.pins.txd_pad_high);
    if (fault=="before-rtc" || fault=="before-sleep")
      assert(!fault_evidence.pins.txd_awake);
    if (fault=="before-rtc") assert(fault_evidence.pins.rtc_touch13_mux_sel);
    if (fault=="before-sleep") assert(fault_evidence.pins.iomux_sleep_sel);
    if (fault=="before-mux") assert(!fault_evidence.pins.txd_mux_gpio);
    if (fault=="before-input") assert(!fault_evidence.pins.txd_input_enabled);
    if (fault=="stopped-rx") assert(!fault_evidence.pins.rx_route);
    // Prior to start there is no controller/running status to claim.
    if (stage=="first_hold" || stage=="before_snapshot") {
      assert(!fault_evidence.pins.controller_listen_only);
      assert(!fault_evidence.pins.running);
    }
    if (stage!="before_snapshot" && stage!="first_hold" &&
        stage!="post_display_unsafe" && stage!="display_init" &&
        stage!="driver_install") assert(calls.back()=="uninstall");
    const std::string first_stage=fault_evidence.stage;
    const auto count=calls.size();
    fake_ms=1000; loop(); // CDC attaches late: fault is repeated, not boot-only.
    const std::string line="#PASSIVE_FAULT,stage="+stage+",field="+field+",code=";
    assert(Serial.output.find(line)!=std::string::npos);
    assert(Serial.output.find("txd_pad_high=")!=std::string::npos);
    assert(Serial.output.find("#PASSIVE_BOOT,first_gpio=")!=std::string::npos);
    assert(Serial.output.find("txd_awake=")!=std::string::npos);
    assert(Serial.output.find("controller_listen_only=")!=std::string::npos);
    assert(Serial.output.find("#PASSIVE,git=")==std::string::npos);
    Serial.output.clear(); Serial.input="STATS\n"; loop();
    assert(Serial.output.find(line)!=std::string::npos);
    assert(calls.size()==count && first_stage==fault_evidence.stage);
    return 0;
  }
  assert(fresh(readStats()) && calls.back()=="task");
  if (fault=="display-clobber") {
    assert(display_reasserted);
    assert(first_hold_observation.txd_gpio && first_hold_observation.txd_enabled);
    assert(!display_observation.txd_gpio && !display_observation.txd_enabled);
    assert(display_observation.txd_latch_high && display_observation.txd_pad_high);
    const auto final_snapshot=snapshot(true);
    assert(passive::safe(final_snapshot));
    printBootEvidence();
    assert(Serial.output.find("reassert=1")!=std::string::npos);
  } else assert(!display_reasserted);
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
  else if (fault=="output" || fault=="display-clobber") GPIO.out=0;
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
  else if (fault=="reset") TWAI.mode_reg.rm=true;
  else if (fault=="self_test") TWAI.mode_reg.stm=true;
  else if (fault=="state") fake_info.state=TWAI_STATE_STOPPED;
  else if (fault=="status") status_fail=true;
  else if (fault=="late") fake_ms=301;
  else assert(false);
  assert(!check());
  assert(stats.fault && !stats.verified && !installed);
  assert(std::string(fault_evidence.stage)=="periodic_check");
  assert(calls[calls.size()-2]=="stop" && calls.back()=="uninstall");
  for (auto& r:GPIO.func_out_sel_cfg) assert(r.func_sel!=TWAI_TX_IDX);
  assert(GPIO.out&(1U<<13));
  const auto n=calls.size();
  assert(!check()); assert(calls.size()==n); // No restart, no second uninstall.
  loop();
  assert(Serial.output.find("#PASSIVE_FAULT,CAN_OFF_LATCHED")!=std::string::npos);
}
