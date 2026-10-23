#include "../src/ble_link_edge_trace_model.h"
#include <cassert>
#include <cstdio>

int main() {
  m5can::BleLinkEdgeTrace trace{};
  assert(trace.gap_connect_events==0 && trace.gap_connect_failed==0);
  assert(trace.last_connect_status==-1 && trace.last_disconnect_reason==-1);

  // An accepted peripheral GAP connect is distinct from GATT peer activity.
  trace.connect(120,0,7);
  assert(trace.gap_connect_events==1 && trace.gap_connect_ok==1);
  assert(trace.gap_connect_failed==0);
  assert(trace.last_connect_handle==7 && trace.last_connect_ms==120);

  // A failed GAP connection need not call NimBLEServer::onConnect.
  trace.connect(245,574,0);
  assert(trace.gap_connect_events==2 && trace.gap_connect_failed==1);
  assert(trace.last_connect_status==574 && trace.last_connect_handle==0xffff);
  assert(trace.first_failure_status==574 && trace.first_failure_ms==245);

  trace.connect(270,11,99);
  assert(trace.gap_connect_failed==2);
  assert(trace.first_failure_status==574 && trace.first_failure_ms==245);

  // A later early disconnect must retain its HCI reason and conn handle.
  trace.disconnect(340,574,7);
  assert(trace.gap_disconnect_events==1);
  assert(trace.last_disconnect_reason==574);
  assert(trace.last_disconnect_ms==340 && trace.last_disconnect_handle==7);

  trace.connect(480,0,9);
  assert(trace.gap_connect_events==4 && trace.gap_connect_ok==2);
  assert(trace.last_connect_status==0 && trace.last_connect_handle==9);
  assert(trace.first_failure_status==574); // no reset across attempts
  std::puts("PASS M5 fixed-RAM GAP connect and disconnect trace model");
}
