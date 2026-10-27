#pragma once
#if defined(M5CAN_SIGNAL_V2_EXPERIMENTAL)
#include <cstdint>
class NimBLEServer;
struct ble_gap_event;
namespace signal_v2 {
bool begin(NimBLEServer *);
void connected(uint16_t);
void disconnected(uint16_t);
void gap(ble_gap_event *);
void poll();
bool usbCommand(const char *);
} // namespace signal_v2
#endif
