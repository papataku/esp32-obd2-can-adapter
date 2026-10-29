#pragma once
// Serial HIL (N1 PR-D substitute, owner decision 2026-10-29): the production
// CanMonitor/CanOwner/budget/epoch code runs unchanged on the M5Dial while
// this bridge replaces ONLY the TWAI driver calls. The TWAI peripheral and its
// GPIOs are never initialized, so no frame can reach a physical CAN bus.
// TX frames are printed as "#HIL_TX,..."; a PC virtual ECU injects RX frames
// and driver faults with "HIL ..." USB lines.
#if defined(M5CAN_SIGNAL_V2_SERIAL_HIL)
#include <driver/twai.h>
namespace hil {
esp_err_t driver_install(const twai_general_config_t*,const twai_timing_config_t*,
                         const twai_filter_config_t*);
esp_err_t driver_uninstall();
esp_err_t start();
esp_err_t stop();
esp_err_t transmit(const twai_message_t* message,TickType_t ticks);
esp_err_t receive(twai_message_t* message,TickType_t ticks);
esp_err_t get_status_info(twai_status_info_t* info);
// USB line handler; true when the line was a HIL command (consumed).
bool usbCommand(const char* line);
} // namespace hil
#if defined(M5CAN_TWAI_HIL_REDIRECT)
#define twai_driver_install hil::driver_install
#define twai_driver_uninstall hil::driver_uninstall
#define twai_start hil::start
#define twai_stop hil::stop
#define twai_transmit hil::transmit
#define twai_receive hil::receive
#define twai_get_status_info hil::get_status_info
#endif
#endif
