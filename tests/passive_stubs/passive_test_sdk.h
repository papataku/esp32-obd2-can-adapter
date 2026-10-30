#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <string>
#include <cassert>
#include <vector>
#define CONFIG_IDF_TARGET_ESP32S3 1
#define SOC_GPIO_PIN_COUNT 49
#define SIG_GPIO_OUT_IDX 256
#define TWAI_TX_IDX 116
#define TWAI_RX_IDX 116
#define MCU_SEL_S 12
#define MCU_SEL_V 7
#define PIN_FUNC_GPIO 1
#define FUN_IE (1U<<9)
#define SLP_SEL 2
#define RTC_IO_TOUCH_PAD13_REG 1000
#define RTC_IO_TOUCH_PAD13_MUX_SEL (1U<<19)
#define REG_READ(x) registers[x]
inline uint32_t registers[1001]{};
inline uint32_t GPIO_PIN_MUX_REG[49]{};
union OutRoute { uint32_t val=0; struct { uint32_t func_sel:9, inv_sel:1, oen_sel:1, oen_inv_sel:1, reserved:20; }; };
struct Gpio { uint32_t enable=0,out=0; struct { bool pad_driver=false; } pin[49]; OutRoute func_out_sel_cfg[49]; struct { uint32_t val=0; } func_in_sel_cfg[256]; };
inline Gpio GPIO;
inline struct { struct { bool lom=false,stm=false,rm=false; } mode_reg; } TWAI;
using gpio_num_t=int;
#define GPIO_MODE_OUTPUT 2
#define GPIO_MODE_INPUT 1
#define GPIO_MODE_INPUT_OUTPUT 3
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
struct gpio_config_t { uint64_t pin_bit_mask=0; int mode=0,pull_up_en=0,pull_down_en=0; };
// Startup stubs deliberately model GPIO13 reset-/RTC-side anomalies that the
// old all-zeros register initialization could not exercise.
inline bool pad_high=true;
inline bool stuck_pad_low=false, display_disables_gpio13=false;
inline bool display_drives_gpio13_low=false, display_corrupts_gpio13_route=false;
inline bool display_clobber_external_low=false;
inline bool mock_spi_bus_fail=false, mock_dma_alloc_fail=false, mock_i2c_begin=false;
inline bool i2c_scl_gpio15_driven=false;
inline int spi_bus_init_calls=0, dma_alloc_calls=0, i2c_begin_calls=0;
inline bool stuck_rtc_mux=false,stuck_sleep=false,bad_iomux=false,bad_input=false;
inline bool bad_rx_on_install=false,skip_listen_on_start=false;
inline int gpio_set_level(int pin,int value) { if (pin<32) GPIO.out=(GPIO.out&~(1U<<pin))|(value<<pin); return 0; }
inline int rtc_gpio_deinit(int) { registers[1000]=stuck_rtc_mux ? RTC_IO_TOUCH_PAD13_MUX_SEL : 0; return 0; }
inline int gpio_config(gpio_config_t*) { GPIO.enable|=1U<<13; registers[13]=((bad_iomux ? 0U : PIN_FUNC_GPIO)<<MCU_SEL_S)|(bad_input ? 0U : FUN_IE)|(stuck_sleep ? SLP_SEL : 0U); pad_high=!stuck_pad_low; return 0; }
inline int gpio_get_level(int) { return pad_high; }
inline void esp_rom_gpio_connect_out_signal(unsigned pin,unsigned sig,bool,bool) { GPIO.func_out_sel_cfg[pin].val=sig; }
inline int gpio_set_direction(int,int) { return 0; }
using esp_err_t=int;
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define ESP_FAIL 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_INVALID_STATE 4
#define SPICOMMON_BUSFLAG_MASTER 1
#define ESP_IDF_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(4,4,7)
#define TWAI_STATE_STOPPED 0
#define TWAI_STATE_RUNNING 1
#define TWAI_MODE_LISTEN_ONLY 2
#define TWAI_ALERT_BUS_ERROR 1
#define TWAI_ALERT_ARB_LOST 2
#define TWAI_ALERT_BUS_OFF 4
struct twai_general_config_t { int tx_io,rx_io,mode,tx_queue_len=1,rx_queue_len=1,alerts_enabled=0; };
#define TWAI_GENERAL_CONFIG_DEFAULT(t,r,m) twai_general_config_t{t,r,m}
struct twai_timing_config_t { int bitrate; };
#define TWAI_TIMING_CONFIG_500KBITS() twai_timing_config_t{500000}
struct twai_filter_config_t { bool accept_all; };
#define TWAI_FILTER_CONFIG_ACCEPT_ALL() twai_filter_config_t{true}
struct twai_status_info_t { uint32_t bus_error_count=0,arb_lost_count=0,rx_missed_count=0,rx_overrun_count=0,rx_error_counter=0,tx_error_counter=0; int state=TWAI_STATE_RUNNING; };
struct twai_message_t { bool extd=false; };
inline bool install_fail=false,start_fail=false,status_fail=false;
inline twai_status_info_t fake_info;
inline std::vector<std::string> calls;
inline int twai_driver_install(const twai_general_config_t* g,const twai_timing_config_t* t,const twai_filter_config_t* f) {
  assert(g->tx_io==39 && g->rx_io==15 && g->mode==TWAI_MODE_LISTEN_ONLY);
  assert(g->tx_queue_len==0 && g->rx_queue_len>0 && t->bitrate==500000 && f->accept_all);
  assert(GPIO.func_out_sel_cfg[13].func_sel==SIG_GPIO_OUT_IDX);
  assert(GPIO.out&(1U<<13)); calls.push_back("install");
  if (install_fail) return ESP_FAIL;
  GPIO.func_out_sel_cfg[39].func_sel=TWAI_TX_IDX;
  GPIO.func_in_sel_cfg[TWAI_RX_IDX].val=(bad_rx_on_install ? 14 : 15)|(1U<<7);
  return ESP_OK;
}
inline int twai_start() {
  for (auto& route:GPIO.func_out_sel_cfg) assert(route.func_sel!=TWAI_TX_IDX);
  assert(GPIO.out&(1U<<13)); assert(GPIO.enable&(1U<<13));
  TWAI.mode_reg.lom=!skip_listen_on_start; calls.push_back("start"); return start_fail?ESP_FAIL:ESP_OK;
}
inline int twai_stop() { calls.push_back("stop"); return ESP_OK; }
inline int twai_driver_uninstall() { calls.push_back("uninstall"); return ESP_OK; }
inline int twai_get_status_info(twai_status_info_t* i) { *i=fake_info; return status_fail?ESP_FAIL:ESP_OK; }
inline int receive_result=ESP_ERR_TIMEOUT;
inline bool receive_extended=false;
inline int twai_receive(twai_message_t* m,int) { m->extd=receive_extended; return receive_result; }
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) (void)(x)
#define portEXIT_CRITICAL(x) (void)(x)
#define pdMS_TO_TICKS(x) (x)
#define pdPASS 1
#define configMAX_PRIORITIES 25
inline uint32_t fake_ms=0;
inline uint32_t millis() { return fake_ms; }
inline void delay(int n) { fake_ms+=n; }
inline void vTaskDelay(int) {}
inline bool task_fail=false;
inline int xTaskCreatePinnedToCore(void(*)(void*),const char*,int,void*,int,void*,int) { calls.push_back("task"); return task_fail?0:pdPASS; }
inline struct FakeSerial {
  std::string input,output;
  void begin(int) {} void setTxTimeoutMs(int) {}
  int available() { return int(input.size()); }
  char read() { char c=input[0]; input.erase(0,1); return c; }
  void println(const char* s) { output+=std::string(s)+"\n"; }
  template<class... A> void printf(const char* f,A... a) { char b[2048]; std::snprintf(b,sizeof(b),f,a...); output+=b; }
} Serial;
namespace fonts { inline int Font2=0; }
#define TFT_BLACK 0
#define TFT_GREEN 1
#define TFT_RED 2
#define TFT_WHITE 3
#define middle_center 0
#define SPI2_HOST 2
#define SPI3_HOST 3
#define SPI_DMA_CH_AUTO 3
#define DEV_NUM_MAX 6
struct FakeSpiLock {};
struct FakeSpiDev {};
using spi_bus_lock_handle_t=FakeSpiLock*;
using spi_bus_lock_dev_handle_t=FakeSpiDev*;
struct spi_bus_lock_dev_config_t { unsigned flags=0; };
inline FakeSpiLock mock_spi3_lock;
inline FakeSpiDev mock_spi3_dev;
inline int mock_spi3_registered_devices;
inline spi_bus_lock_handle_t spi_bus_lock_get_by_id(int host) {
  assert(host==SPI3_HOST);
  return &mock_spi3_lock;
}
inline int spi_bus_lock_register_dev(spi_bus_lock_handle_t lock,
                                     spi_bus_lock_dev_config_t* cfg,
                                     spi_bus_lock_dev_handle_t* out) {
  assert(lock==&mock_spi3_lock && cfg && cfg->flags==0 && out);
  *out=&mock_spi3_dev;
  return ESP_OK;
}
inline int spi_bus_lock_get_dev_id(spi_bus_lock_dev_handle_t dev) {
  assert(dev==&mock_spi3_dev);
  return DEV_NUM_MAX-1-mock_spi3_registered_devices;
}
inline void spi_bus_lock_unregister_dev(spi_bus_lock_dev_handle_t dev) {
  assert(dev==&mock_spi3_dev);
}
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA 2
struct spi_bus_config_t {
  int mosi_io_num=-1,miso_io_num=-1,sclk_io_num=-1;
  int quadwp_io_num=-1,quadhd_io_num=-1,max_transfer_sz=0,flags=0;
  int data4_io_num=-1,data5_io_num=-1,data6_io_num=-1,data7_io_num=-1,intr_flags=0;
};
inline int spi_bus_initialize(int host,const spi_bus_config_t* cfg,int dma) {
  ++spi_bus_init_calls;
  assert(host==SPI3_HOST && cfg->mosi_io_num==5 && cfg->miso_io_num==-1);
  assert(cfg->sclk_io_num==6 && cfg->quadwp_io_num==-1 && cfg->quadhd_io_num==-1);
  assert(cfg->data4_io_num==-1 && cfg->data5_io_num==-1 &&
         cfg->data6_io_num==-1 && cfg->data7_io_num==-1);
  assert(dma==SPI_DMA_CH_AUTO);
  if (mock_spi_bus_fail) return ESP_FAIL;
  // M5GFX silently tolerates INVALID_STATE on its redundant second init.
  return spi_bus_init_calls==1?ESP_OK:ESP_ERR_INVALID_STATE;
}
inline struct FakeSpiRegisteredDevice {} fake_spi_registered_device;
using spi_device_handle_t=FakeSpiRegisteredDevice*;
struct spi_device_interface_config_t {
  int clock_speed_hz=0,spics_io_num=-1,queue_size=0,flags=0;
};
inline bool mock_m5gfx_device_add_fail=false;
inline int mock_spi_add_device_calls=0;
inline int spi_bus_add_device(int host,const spi_device_interface_config_t* cfg,
                              spi_device_handle_t* handle) {
  ++mock_spi_add_device_calls;
  assert(host==SPI3_HOST && cfg->clock_speed_hz==10000000);
  assert(cfg->spics_io_num==-1 && cfg->queue_size==1 && handle);
  if (mock_m5gfx_device_add_fail) return ESP_ERR_NO_MEM;
  *handle=&fake_spi_registered_device;
  ++mock_spi3_registered_devices;
  return ESP_OK;
}
inline uint8_t dma_probe_storage[512]{};
inline void* heap_caps_malloc(size_t len,uint32_t caps) {
  ++dma_alloc_calls;
  assert(len>=480 && (caps & MALLOC_CAP_INTERNAL) && (caps & MALLOC_CAP_DMA));
  return mock_dma_alloc_fail?nullptr:dma_probe_storage;
}
inline void heap_caps_free(void* p) { assert(p==dma_probe_storage); }
inline void simulate_wire_begin() {
  ++i2c_begin_calls;
  // StampS3 defaults: SDA=GPIO13, SCL=GPIO15. The injected call
  // also drives SCL so the model covers both protected physical pins.
  GPIO.enable &= ~(1U<<13);
  GPIO.func_out_sel_cfg[13].val=0;
  pad_high=false;
  GPIO.enable |= (1U<<15);
  GPIO.out &= ~(1U<<15);
  i2c_scl_gpio15_driven=true;
}
// Model Arduino-ESP32 2.0.17 SPI2 MISO=-1 -> GPIO13.
inline int lcd_spi_host=-1;
inline bool mock_display_dma_fail=false;
// A successful M5GFX init is not proof of IDF SPI3 device registration.
inline bool mock_m5gfx_second_bus_invalid_state=false;
// Registration belongs to the M5GFX device, never the transient probe.
inline bool lcd_auto_attached_gpio13=false;
namespace lgfx {
struct Bus_SPI {
  struct Config { int spi_host,spi_mode,freq_write,freq_read,dma_channel,pin_sclk,pin_mosi,pin_miso,pin_dc; bool spi_3wire,use_lock; };
  Config config() { return {}; }
  bool reserveDMABuffer(unsigned len) { assert(len==480); return !mock_display_dma_fail; }
  uint8_t* getDMABuffer(unsigned len) {
    assert(len==480);
    return mock_display_dma_fail ? nullptr : dma_probe_storage;
  }
  void config(Config c) {
    assert(c.spi_host==SPI2_HOST || c.spi_host==SPI3_HOST);
    assert(c.pin_sclk==6 && c.pin_mosi==5 && c.pin_miso==-1 &&
           c.pin_dc==4 && c.spi_3wire);
    lcd_spi_host=c.spi_host;
  }
};
struct Light_PWM {
  struct Config { int pin_bl,freq,pwm_channel; bool invert; };
  Config config() { return {}; } void config(Config c) { assert(c.pin_bl==9); }
};
struct Panel_GC9A01 {
  struct Config { int pin_cs,pin_rst,pin_busy,panel_width,panel_height; bool readable,invert,rgb_order; };
  Config config() { return {}; } void config(Config c) { assert(c.pin_cs==7 && c.pin_rst==8); }
  void setBus(Bus_SPI*) {} void setLight(Light_PWM*) {}
};
inline bool display_ok=true;
class LGFX_Device {
 protected:
  Bus_SPI* bus_=nullptr;
 public:
  void setPanel(Panel_GC9A01*) {}
  bool init() {
    assert(lcd_spi_host==SPI2_HOST || lcd_spi_host==SPI3_HOST);
    if (lcd_spi_host==SPI2_HOST) {
      // Arduino SPIClass::begin(6,-1,5) invokes spiAttachMISO(-1).
      // On ESP32-S3 FSPI -1 selects GPIO13 and pinMode(INPUT).
      GPIO.enable &= ~(1U<<13);
      GPIO.func_out_sel_cfg[13].val=0;
      pad_high=false;
      lcd_auto_attached_gpio13=true;
    }
    // M5GFX 0.2.35 attempts the same bus initialization for a second
    // time, ignores INVALID_STATE, then independently registers its device.
    spi_bus_config_t redundant{};
    redundant.mosi_io_num=5;
    redundant.miso_io_num=-1;
    redundant.sclk_io_num=6;
    const int second_status=spi_bus_initialize(SPI3_HOST,&redundant,SPI_DMA_CH_AUTO);
    assert(second_status==ESP_ERR_INVALID_STATE);
    if (mock_m5gfx_second_bus_invalid_state) assert(spi_bus_init_calls==2);
    spi_device_interface_config_t devcfg{};
    devcfg.clock_speed_hz=10000000;
    devcfg.spics_io_num=-1;
    devcfg.queue_size=1;
    spi_device_handle_t handle=nullptr;
    (void)spi_bus_add_device(SPI3_HOST,&devcfg,&handle); // M5GFX ignores result
    if (mock_i2c_begin) simulate_wire_begin();
    if (display_disables_gpio13) {
      if (display_clobber_external_low) stuck_pad_low=true;
      assert(GPIO.out & (1U<<13));
      GPIO.enable &= ~(1U<<13);
      GPIO.func_out_sel_cfg[13].val=0;
      // Powered U179 may retain pad HIGH via its TXD pull-up. Emulate
      // an independently LOW pad with display_clobber_external_low.
      pad_high=!display_clobber_external_low;
    }
    if (display_drives_gpio13_low) {
      GPIO.out &= ~(1U<<13);
      pad_high=false;
    }
    if (display_corrupts_gpio13_route)
      GPIO.func_out_sel_cfg[13].val=42;
    return display_ok;
  }
  void setBrightness(int) {}
  void fillScreen(int) { assert(display_ok); } void setTextDatum(int) {} void setTextColor(int,int) {}
  void drawString(const char*,int,int,int*) {}
};
}
