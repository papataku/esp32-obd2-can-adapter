#pragma once
#include <cstdint>

// Pure, fixed-RAM state. Caller serializes access (ESP32 portMUX).
// No USB/NVS/allocation, and no inference of a physical RF cause.
namespace m5can {
struct BleLinkEdgeTrace {
  uint32_t gap_connect_events=0;
  uint32_t gap_connect_ok=0;
  uint32_t gap_connect_failed=0;
  uint32_t gap_disconnect_events=0;
  uint32_t last_connect_ms=0, last_disconnect_ms=0;
  uint32_t first_failure_ms=0;
  int32_t last_connect_status=-1, first_failure_status=-1;
  int32_t last_disconnect_reason=-1;
  uint16_t last_connect_handle=0xffff, last_disconnect_handle=0xffff;

  void connect(uint32_t now_ms, int32_t status, uint16_t handle) {
    ++gap_connect_events;
    last_connect_ms=now_ms;
    last_connect_status=status;
    last_connect_handle=status==0 ? handle : 0xffff;
    if(status==0) {
      ++gap_connect_ok;
    } else {
      ++gap_connect_failed;
      if(gap_connect_failed==1) {
        first_failure_ms=now_ms;
        first_failure_status=status;
      }
    }
  }

  void disconnect(uint32_t now_ms, int32_t reason, uint16_t handle) {
    ++gap_disconnect_events;
    last_disconnect_ms=now_ms;
    last_disconnect_reason=reason;
    last_disconnect_handle=handle;
  }
};
} // namespace m5can
