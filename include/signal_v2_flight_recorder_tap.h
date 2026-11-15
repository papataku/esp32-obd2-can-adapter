#pragma once
// Flight recorder result tap (PM Claude 2026-11-15). C++11, header only.
// Written ONLY by CanOwner::tick (BLE app owner task) AFTER the completed
// result was handled, from const references. Read by the recorder producer in
// the same task right after the tick. It is never an input to CAN admission.
#include <cstdint>
namespace signal_v2 {
namespace recorder {
struct ResultTap {
  // First-response latency buckets (ms): <10,<20,<30,<50,<80,<120,>=120, none.
  static constexpr unsigned kBuckets = 8;
  uint32_t results = 0, latency[kBuckets] = {}, status[10] = {};
  uint32_t nodata_run = 0, nodata_run_max = 0, multi_frame = 0;
  uint32_t last_first_us = 0, last_transaction_us = 0, last_ff_us = 0, last_cf_us = 0;
  uint8_t last_status = 0, last_responses = 0, last_raw = 0, last_sf = 0, last_ff = 0,
          last_cf = 0, ecu_state = 0, ecu_bad = 0;
};
inline ResultTap &resultTap() {
  static ResultTap tap;
  return tap;
}
inline void tapAdd(uint32_t &v) { if (v != 0xFFFFFFFFu) ++v; }
template <class Result, class Selector>
inline void tapResult(const Result &r, const Selector &ecu) {
  ResultTap &t = resultTap();
  tapAdd(t.results);
  const unsigned status = static_cast<unsigned>(r.status);
  t.last_status = static_cast<uint8_t>(status);
  if (status < 10) tapAdd(t.status[status]);
  if (status == 1) {  // DiagnosticStatus::NoData
    tapAdd(t.nodata_run);
    if (t.nodata_run > t.nodata_run_max) t.nodata_run_max = t.nodata_run;
  } else {
    t.nodata_run = 0;
  }
  t.last_responses = r.response_count;
  t.last_raw = r.raw_frame_count;
  t.last_first_us = r.tx_to_first_response_us;
  t.last_transaction_us = r.transaction_us;
  t.last_sf = t.last_ff = t.last_cf = 0;
  t.last_ff_us = t.last_cf_us = 0;
  for (unsigned i = 0; i < r.raw_frame_count; ++i) {
    const unsigned pci = r.raw_frames[i].dlc ? (r.raw_frames[i].data[0] >> 4) : 0xF;
    if (pci == 0) ++t.last_sf;
    else if (pci == 1) { ++t.last_ff; t.last_ff_us = r.raw_rx_offsets_us[i]; }
    else if (pci == 2) { ++t.last_cf; t.last_cf_us = r.raw_rx_offsets_us[i]; }
  }
  if (t.last_ff) tapAdd(t.multi_frame);
  unsigned b = ResultTap::kBuckets - 1;
  if (r.raw_frame_count && r.tx_to_first_response_us) {
    const uint32_t ms = r.tx_to_first_response_us / 1000;
    b = ms < 10 ? 0 : ms < 20 ? 1 : ms < 30 ? 2 : ms < 50 ? 3 : ms < 80 ? 4 : ms < 120 ? 5 : 6;
  }
  tapAdd(t.latency[b]);
  t.ecu_state = static_cast<uint8_t>(ecu.state());
  t.ecu_bad = ecu.consecutiveBad();
}
}  // namespace recorder
}  // namespace signal_v2
