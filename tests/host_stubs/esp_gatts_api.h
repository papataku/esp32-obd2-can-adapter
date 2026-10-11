#pragma once
#include <cstdint>
using esp_err_t = int;
static constexpr esp_err_t ESP_OK = 0;
struct esp_ble_gatts_cb_param_t {
  struct { uint16_t conn_id = 0; } connect;
  struct { uint16_t conn_id = 0; } disconnect;
  struct {
    uint16_t conn_id = 0;
    uint16_t len = 0;
    uint8_t* value = nullptr;
  } write;
  struct { uint16_t conn_id = 0; uint16_t mtu = 23; } mtu;
};
inline esp_err_t esp_ble_gatts_send_indicate(
    uint8_t, uint16_t, uint16_t, uint16_t, uint8_t*, bool) { return ESP_OK; }
