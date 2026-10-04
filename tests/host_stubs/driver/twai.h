#pragma once
#include <cstdint>
#include "Arduino.h"
using esp_err_t=int;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE -2
using twai_mode_t=int;
#define TWAI_MODE_LISTEN_ONLY 0
#define TWAI_MODE_NORMAL 1
typedef enum {TWAI_STATE_STOPPED=0,TWAI_STATE_RUNNING=1,TWAI_STATE_BUS_OFF=2,TWAI_STATE_RECOVERING=3} twai_state_t;
struct twai_general_config_t{uint32_t tx_queue_len=0,rx_queue_len=0,alerts_enabled=0;};
struct twai_timing_config_t{};
struct twai_filter_config_t{};
inline twai_general_config_t twai_general_config_default_stub(int,int,twai_mode_t){return {};}
#define TWAI_GENERAL_CONFIG_DEFAULT(tx,rx,mode) twai_general_config_default_stub((tx),(rx),(mode))
#define TWAI_TIMING_CONFIG_500KBITS() twai_timing_config_t{}
#define TWAI_FILTER_CONFIG_ACCEPT_ALL() twai_filter_config_t{}
#define TWAI_ALERT_RX_QUEUE_FULL 1
#define TWAI_ALERT_BUS_OFF 2
#define TWAI_ALERT_BUS_RECOVERED 4
#define TWAI_ALERT_ERR_PASS 8
#define TWAI_ALERT_ABOVE_ERR_WARN 16
#define TWAI_ALERT_BELOW_ERR_WARN 32
struct twai_status_info_t{uint32_t rx_missed_count=0,rx_overrun_count=0,arb_lost_count=0,bus_error_count=0,msgs_to_rx=0;twai_state_t state=TWAI_STATE_RUNNING;};
struct twai_message_t{
  uint32_t extd=0,rtr=0,ss=0,self=0,identifier=0;
  uint8_t data_length_code=0;
  uint8_t data[8]{};
};
inline esp_err_t twai_driver_install(const twai_general_config_t*,const twai_timing_config_t*,const twai_filter_config_t*){return ESP_OK;}
inline esp_err_t twai_driver_uninstall(){return ESP_OK;}
inline esp_err_t twai_start(){return ESP_OK;}
inline esp_err_t twai_stop(){return ESP_OK;}
inline esp_err_t twai_receive(twai_message_t*,TickType_t){return ESP_FAIL;}
inline esp_err_t twai_transmit(const twai_message_t*,TickType_t){return ESP_OK;}
inline esp_err_t twai_get_status_info(twai_status_info_t*){return ESP_OK;}
