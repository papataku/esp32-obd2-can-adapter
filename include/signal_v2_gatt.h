#pragma once
#if defined(M5CAN_SIGNAL_V2_EXPERIMENTAL)
#include <cstdint>
class NimBLEServer;
namespace m5can { class CanMonitor; }
struct ble_gap_event;
namespace signal_v2 {
bool begin(NimBLEServer *);
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
void attachCan(m5can::CanMonitor*);
#if defined(M5CAN_SIGNAL_V2_OWNER_CONTINUOUS_LIVE)
struct CanTxDisplay { bool armed, busy; const char* reason; bool manual; };
#if defined(M5CAN_SIGNAL_V2_OWNER_AUTO_ARM)
void manualCanStop();
#endif
CanTxDisplay canTxDisplay();
#endif
#endif
void connected(uint16_t);
void disconnected(uint16_t);
void gap(ble_gap_event *);
void poll();
bool usbCommand(const char *);
} // namespace signal_v2
#endif
