// This is the ONLY application translation unit in the passive environment.
// Legacy can_monitor (including its NORMAL transition), ARM, leases, BLE and
// diagnostic parsing are excluded by build_src_filter, not a runtime setting.
#if defined(M5CAN_PASSIVE_TX_ISOLATED)
#include <Arduino.h>
#include <M5GFX.h>
#include <lgfx/v1/panel/Panel_GC9A01.hpp>
#include <atomic>
#include <cstring>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_rom_gpio.h>
#include <soc/gpio_struct.h>
#include <soc/gpio_sig_map.h>
#include <soc/io_mux_reg.h>
#include <soc/rtc_io_reg.h>
#include <soc/twai_struct.h>
#include "passive_policy.h"
#ifndef M5CAN_BUILD_SHA
#define M5CAN_BUILD_SHA "unknown"
#endif
static_assert(CONFIG_IDF_TARGET_ESP32S3, "register checks require ESP32-S3");
static_assert(SOC_GPIO_PIN_COUNT == 49, "review GPIO matrix coverage for new SoC");
namespace {
constexpr gpio_num_t txd = static_cast<gpio_num_t>(passive::kTxd);
constexpr gpio_num_t rx = static_cast<gpio_num_t>(passive::kRx);
constexpr gpio_num_t sink = static_cast<gpio_num_t>(passive::kInstallSink);
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
struct Stats {
  uint64_t frames=0, standard=0, extended=0;
  uint32_t bus_errors=0, arb_lost=0, missed=0, overrun=0;
  uint32_t rec=0, tec=0, check_ms=0, checks=0;
  int state=TWAI_STATE_STOPPED, stop_result=0, uninstall_result=0;
  bool verified=false, fault=false;
} stats;
passive::Latch latch; // Accessed only by setup, then the CAN owner.
std::atomic<bool> request_fault{false};
bool installed=false;
uint32_t last_check=0;
// Exact Dial schematic pins. Never call M5.begin / display autodetection:
// cached or failed board detection can probe PORT.A and pulse CAN TXD.
class DialDisplay : public lgfx::LGFX_Device {
  lgfx::Bus_SPI bus_;
  lgfx::Panel_GC9A01 panel_;
  lgfx::Light_PWM light_;
 public:
  DialDisplay() {
    auto b=bus_.config();
    b.spi_host=SPI2_HOST; b.spi_mode=0; b.freq_write=40000000;
    b.freq_read=16000000; b.spi_3wire=true; b.use_lock=true;
    b.dma_channel=SPI_DMA_CH_AUTO;
    b.pin_sclk=6; b.pin_mosi=5; b.pin_miso=-1; b.pin_dc=4;
    bus_.config(b); panel_.setBus(&bus_);
    auto p=panel_.config();
    p.pin_cs=7; p.pin_rst=8; p.pin_busy=-1;
    p.panel_width=240; p.panel_height=240;
    p.readable=false; p.invert=true; p.rgb_order=false;
    panel_.config(p);
    auto l=light_.config(); l.pin_bl=9; l.freq=44100;
    l.pwm_channel=7; l.invert=false;
    light_.config(l); panel_.setLight(&light_);
    setPanel(&panel_);
  }
} display;

void holdTxdHigh() {
  // Set the output latch BEFORE enabling the pad. Reset-time high impedance
  // is biased recessive by the TJA1051T/3's internal TXD pull-up (datasheet 7.2.2).
  gpio_set_level(txd, 1);
  rtc_gpio_deinit(txd);
  gpio_config_t cfg{};
  cfg.pin_bit_mask=1ULL << passive::kTxd;
  cfg.mode=GPIO_MODE_INPUT_OUTPUT;
  cfg.pull_up_en=GPIO_PULLUP_ENABLE;
  cfg.pull_down_en=GPIO_PULLDOWN_DISABLE;
  gpio_config(&cfg);
  esp_rom_gpio_connect_out_signal(txd, SIG_GPIO_OUT_IDX, false, false);
  // Explicit software-controlled output enable, with no inversion.
  GPIO.func_out_sel_cfg[passive::kTxd].val=SIG_GPIO_OUT_IDX | (1U<<10);
}
void detachTx() {
  for (unsigned i=0; i<SOC_GPIO_PIN_COUNT; ++i) {
    if (GPIO.func_out_sel_cfg[i].func_sel == TWAI_TX_IDX)
      esp_rom_gpio_connect_out_signal(i, SIG_GPIO_OUT_IDX, false, false);
  }
}
passive::Snapshot snapshot(bool running) {
  passive::Snapshot s;
  s.no_tx_routes=true;
  for (unsigned i=0; i<SOC_GPIO_PIN_COUNT; ++i)
    if (GPIO.func_out_sel_cfg[i].func_sel == TWAI_TX_IDX) s.no_tx_routes=false;
  const uint32_t route=GPIO.func_out_sel_cfg[passive::kTxd].val;
  s.txd_gpio=(route & 0xFFFU) == (SIG_GPIO_OUT_IDX | (1U<<10));
  s.txd_enabled=(GPIO.enable & (1U<<passive::kTxd)) != 0;
  s.txd_latch_high=(GPIO.out & (1U<<passive::kTxd)) != 0;
  s.txd_pad_high=gpio_get_level(txd) == 1;
  const uint32_t iomux=REG_READ(GPIO_PIN_MUX_REG[passive::kTxd]);
  s.txd_mux_gpio=((iomux >> MCU_SEL_S) & MCU_SEL_V) == PIN_FUNC_GPIO;
  s.txd_input_enabled=(iomux & FUN_IE) != 0;
  s.txd_push_pull=!GPIO.pin[passive::kTxd].pad_driver;
  s.txd_awake=!(iomux & SLP_SEL) &&
      !(REG_READ(RTC_IO_TOUCH_PAD13_REG) & RTC_IO_TOUCH_PAD13_MUX_SEL);
  const auto input=GPIO.func_in_sel_cfg[TWAI_RX_IDX].val;
  s.rx_route=(input & 0xFFU) == (passive::kRx | (1U<<7));
  s.controller_listen_only=installed && TWAI.mode_reg.lom && !TWAI.mode_reg.stm && !TWAI.mode_reg.rm;
  s.running=running;
  return s;
}
void failClosed() {
  latch.fail();
  // Contain a detected routing fault BEFORE touching the driver. A periodic
  // check cannot retroactively prevent a transient or arbitrary memory fault.
  detachTx();
  holdTxdHigh();
  int stopped=ESP_OK, uninstalled=ESP_OK;
  if (installed) {
    stopped=twai_stop();
    uninstalled=twai_driver_uninstall();
    installed=false;
  }
  portENTER_CRITICAL(&mux);
  stats.fault=true; stats.verified=false; stats.state=TWAI_STATE_STOPPED;
  stats.stop_result=stopped; stats.uninstall_result=uninstalled;
  portEXIT_CRITICAL(&mux);
}
bool check() {
  twai_status_info_t info{};
  const bool ok=installed && twai_get_status_info(&info) == ESP_OK;
  const uint32_t now=millis();
  if (request_fault.load() || !latch.check(snapshot(ok && info.state==TWAI_STATE_RUNNING), now)) {
    failClosed(); return false;
  }
  last_check=now;
  portENTER_CRITICAL(&mux);
  stats.verified=true; stats.check_ms=now; ++stats.checks;
  stats.bus_errors=info.bus_error_count; stats.arb_lost=info.arb_lost_count;
  stats.missed=info.rx_missed_count; stats.overrun=info.rx_overrun_count;
  stats.rec=info.rx_error_counter; stats.tec=info.tx_error_counter; stats.state=info.state;
  portEXIT_CRITICAL(&mux);
  return true;
}
void receiveOnce() {
  if (latch.fault()) return;
  if (uint32_t(millis()-last_check) >= passive::kCheckPeriodMs && !check()) return;
  twai_message_t frame{};
  // Bounded wait. No diagnostic queue, TX API, automatic recovery or reinstall.
  const esp_err_t result=twai_receive(&frame, pdMS_TO_TICKS(5));
  if (result==ESP_OK) {
    portENTER_CRITICAL(&mux);
    ++stats.frames;
    if (frame.extd) ++stats.extended; else ++stats.standard;
    portEXIT_CRITICAL(&mux);
  } else if (result!=ESP_ERR_TIMEOUT) failClosed();
}
void owner(void*) {
  for (;;) {
    receiveOnce();
    if (latch.fault()) vTaskDelay(pdMS_TO_TICKS(10));
  }
}
Stats readStats() {
  portENTER_CRITICAL(&mux); const Stats s=stats; portEXIT_CRITICAL(&mux);
  return s;
}
bool fresh(const Stats& s) {
  return s.verified && !s.fault && uint32_t(millis()-s.check_ms)<=passive::kMaxCheckGapMs;
}
void printStats(const Stats& s, uint32_t fps) {
  // Metadata only: no payloads, CAN identifiers, VIN, location or BLE identity.
  Serial.printf("#STATS,profile=PASSIVE_TX_ISOLATED,rx=%llu,rx_s=%lu,std=%llu,ext=%llu,bus_error=%lu,arb_lost=%lu,missed=%lu,overrun=%lu,rec=%lu,tec=%lu,state=%d,self_check=%s,checks=%lu,age_ms=%lu,stop=%d,uninstall=%d\n",
      (unsigned long long)s.frames,(unsigned long)fps,(unsigned long long)s.standard,
      (unsigned long long)s.extended,(unsigned long)s.bus_errors,(unsigned long)s.arb_lost,
      (unsigned long)s.missed,(unsigned long)s.overrun,(unsigned long)s.rec,
      (unsigned long)s.tec,s.state,fresh(s)?"OK":"FAULT",(unsigned long)s.checks,
      (unsigned long)(millis()-s.check_ms),s.stop_result,s.uninstall_result);
}
} // namespace
void setup() {
  holdTxdHigh(); // First application action; no TWAI signal has been attached.
  // Dial's HOLD pin keeps power on; display touches only GPIO4..9.
  gpio_set_level(static_cast<gpio_num_t>(46),1);
  gpio_set_direction(static_cast<gpio_num_t>(46),GPIO_MODE_OUTPUT);
  const bool display_ok=display.init();
  display.setBrightness(128);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(10);
  // A display failure or changed TXD must never allow a verified boot.
  auto before=snapshot(true); before.rx_route=true; before.controller_listen_only=true;
  if (!display_ok || !passive::safe(before)) { failClosed(); return; }
  twai_general_config_t general=TWAI_GENERAL_CONFIG_DEFAULT(sink, rx, TWAI_MODE_LISTEN_ONLY);
  general.tx_queue_len=0; general.rx_queue_len=128;
  general.alerts_enabled=TWAI_ALERT_BUS_ERROR | TWAI_ALERT_ARB_LOST | TWAI_ALERT_BUS_OFF;
  const twai_timing_config_t timing=TWAI_TIMING_CONFIG_500KBITS();
  const twai_filter_config_t filter=TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if (twai_driver_install(&general, &timing, &filter)!=ESP_OK) {
    failClosed(); return;
  }
  installed=true;
  // IDF requires a real tx_io for install. GPIO39 goes only to the unused
  // header P4.10 on Dial; remove that route while the controller is stopped.
  // GPIO13 is NEVER supplied to the TWAI driver.
  detachTx();
  gpio_set_direction(sink, GPIO_MODE_INPUT);
  auto stopped=snapshot(true); stopped.controller_listen_only=true;
  if (!passive::safe(stopped) || twai_start()!=ESP_OK || !check()) {
    failClosed(); return;
  }
  if (xTaskCreatePinnedToCore(owner,"passive-can",4096,nullptr,configMAX_PRIORITIES-3,nullptr,0)!=pdPASS) {
    failClosed(); return;
  }
  Serial.printf("#PASSIVE,git=%s,baud=500000,filter=ALL,tx=ISOLATED\n", M5CAN_BUILD_SHA);
}
void loop() {
  static uint32_t report_ms=0, draw_ms=0, fps=0;
  static uint64_t previous_frames=0;
  static bool fault_logged=false;
  const uint32_t now=millis();
  const Stats s=readStats();
  const bool verified=fresh(s);
  if (!verified) request_fault.store(true);
  if (!verified && !fault_logged) {
    Serial.println("#PASSIVE_FAULT,CAN_OFF_LATCHED,disconnect_vehicle"); fault_logged=true;
  }
  if (uint32_t(now-report_ms)>=1000) {
    fps=uint32_t((s.frames-previous_frames)*1000ULL/uint32_t(now-report_ms));
    report_ms=now; previous_frames=s.frames; printStats(s,fps);
  }
  // Fixed-length command buffer, bounded work per loop. Overlong input is
  // discarded through newline, never interpreted as a suffix command.
  static char command[64]; static size_t used=0; static bool overflow=false;
  for (unsigned n=0;n<64 && Serial.available();++n) {
    const char c=Serial.read();
    if (c=='\r') continue;
    if (c=='\n') {
      command[used]='\0';
      if (!overflow && !std::strcmp(command,"STATS")) printStats(s,fps);
      else Serial.println("#REJECT,PASSIVE_IMMUTABLE,ARM_LEASE_DEMO_TX_UNAVAILABLE");
      used=0; overflow=false;
    } else if (used<sizeof(command)-1 && !overflow) command[used++]=c;
    else overflow=true;
  }
  if (uint32_t(now-draw_ms)>=100) {
    draw_ms=now;
    auto& d=display;
    d.fillScreen(TFT_BLACK); d.setTextDatum(middle_center);
    d.setTextColor(verified?TFT_GREEN:TFT_RED,TFT_BLACK);
    // Font2 keeps the complete required string inside the round display.
    d.drawString(verified?"PASSIVE (TX ISOLATED)":"PASSIVE FAULT",120,85,&fonts::Font2);
    d.setTextColor(TFT_WHITE,TFT_BLACK);
    d.drawString(verified?"500k  RX ONLY / 11+29bit":"CAN OFF - DISCONNECT",120,115,&fonts::Font2);
    char line[40]; snprintf(line,sizeof(line),"RX %lu/s",(unsigned long)fps);
    d.drawString(line,120,145,&fonts::Font2);
  }
  delay(1);
}
#endif
