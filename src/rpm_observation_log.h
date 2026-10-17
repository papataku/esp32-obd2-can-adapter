#pragma once
#include <cstddef>
#include <cstdint>

namespace m5can {
// Host-testable allocation-free RAM-only core. CAN/BLE instrumentation,
// flash storage and USB readout are separate later phases.
enum class RpmOrigin : uint8_t {
  FreshCan=0, CanOwnerCache=1, CoordinatorCache=2,
  Synthetic=3, Unknown=4, Count=5
};
enum class RpmOutcome : uint8_t {
  Ok=0, NoData=1, NegativeResponse=2, Timeout=3, TransportError=4
};
struct RpmResponder {
  uint32_t can_id=0;
  uint16_t quarter_rpm=0; // Raw Mode01 41 0C integer, not rounded RPM
  uint8_t valid=0, reserved=0;
};
struct RpmObservation {
  uint64_t observed_us=0, acquired_us=0, acquisition_id=0;
  uint32_t request_can_id=0, peer=0, peer_generation=0;
  uint8_t service=0, pid=0, grouped=0;
  RpmOrigin origin=RpmOrigin::Unknown;
  RpmOutcome outcome=RpmOutcome::NoData;
  uint8_t physical_tx=0, responder_count=0;
  RpmResponder responders[8]{};
};
struct RpmRun {
  uint64_t first_sequence=0,last_sequence=0;
  uint64_t first_observed_us=0,last_observed_us=0;
  uint64_t first_acquired_us=0,last_acquired_us=0;
  uint64_t first_acquisition_id=0,last_acquisition_id=0;
  uint32_t first_peer=0,last_peer=0;
  uint32_t first_generation=0,last_generation=0;
  uint32_t request_can_id=0;
  uint32_t observations=0,physical_tx_count=0;
  uint32_t min_interval_us=0,max_interval_us=0;
  uint32_t by_origin[static_cast<size_t>(RpmOrigin::Count)]{};
  uint8_t service=0,pid=0,grouped=0;
  RpmOutcome outcome=RpmOutcome::NoData;
  uint8_t responder_count=0,peer_changed=0;
  RpmResponder responders[8]{};
};

// Single-owner API: append from one low-cost collector, never from flash IO.
// Loss, rejected records and sequence holes remain externally observable.
class RpmRunCompressor {
 public:
  static constexpr size_t kRuns=64;
  static constexpr uint8_t kMaxResponders=8;
  struct Stats {
    uint64_t accepted=0,rejected=0,overwritten_runs=0;
    uint64_t clock_regressions=0,last_sequence=0;
  };
  bool append(RpmObservation sample) {
    const uint64_t seq=++stats_.last_sequence;
    if (!validateAndNormalize(sample)) {
      ++stats_.rejected;
      gap_since_last_=true;
      return false;
    }
    ++stats_.accepted;
    if (count_) {
      RpmRun& active=runs_[(head_+count_-1)%kRuns];
      if (sample.observed_us<active.last_observed_us) {
        ++stats_.clock_regressions;
      } else if (!gap_since_last_ && sameResponseSet(active,sample)) {
        const uint64_t delta=sample.observed_us-active.last_observed_us;
        const uint32_t interval=delta>UINT32_MAX?UINT32_MAX:
            static_cast<uint32_t>(delta);
        if (active.observations==1 || interval<active.min_interval_us)
          active.min_interval_us=interval;
        if (interval>active.max_interval_us)
          active.max_interval_us=interval;
        active.last_sequence=seq;
        active.last_observed_us=sample.observed_us;
        active.last_acquired_us=sample.acquired_us;
        active.last_acquisition_id=sample.acquisition_id;
        if (sample.peer!=active.first_peer ||
            sample.peer_generation!=active.first_generation)
          active.peer_changed=1;
        active.last_peer=sample.peer;
        active.last_generation=sample.peer_generation;
        ++active.observations;
        ++active.by_origin[static_cast<size_t>(sample.origin)];
        // Count a physical TX at most once for the same acquisition ID.
        // Unknown IDs cannot safely be classified as unique transactions.
        if (sample.origin==RpmOrigin::FreshCan && sample.physical_tx &&
            sample.acquisition_id && sample.acquisition_id!=prev_physical_id_) {
          ++active.physical_tx_count;
          prev_physical_id_=sample.acquisition_id;
        }
        return true;
      }
    }
    if (count_==kRuns) {
      ++stats_.overwritten_runs;
      head_=(head_+1)%kRuns;
      --count_;
    }
    RpmRun& r=runs_[(head_+count_)%kRuns];
    r=RpmRun{};
    r.first_sequence=r.last_sequence=seq;
    r.first_observed_us=r.last_observed_us=sample.observed_us;
    r.first_acquired_us=r.last_acquired_us=sample.acquired_us;
    r.first_acquisition_id=r.last_acquisition_id=sample.acquisition_id;
    r.first_peer=r.last_peer=sample.peer;
    r.first_generation=r.last_generation=sample.peer_generation;
    r.request_can_id=sample.request_can_id;
    r.service=sample.service;
    r.pid=sample.pid;
    r.grouped=sample.grouped;
    r.outcome=sample.outcome;
    r.observations=1;
    r.responder_count=sample.responder_count;
    for (size_t i=0;i<sample.responder_count;++i)
      r.responders[i]=sample.responders[i];
    ++r.by_origin[static_cast<size_t>(sample.origin)];
    if (sample.origin==RpmOrigin::FreshCan && sample.physical_tx &&
        sample.acquisition_id && sample.acquisition_id!=prev_physical_id_) {
      r.physical_tx_count=1;
      prev_physical_id_=sample.acquisition_id;
    }
    ++count_;
    gap_since_last_=false;
    return true;
  }
  const RpmRun* at(size_t index) const {
    if (index>=count_) return nullptr;
    return &runs_[(head_+index)%kRuns];
  }
  size_t size() const { return count_; }
  Stats stats() const { return stats_; }

 private:
  static bool validateAndNormalize(RpmObservation& v) {
    if (v.service!=0x01 || v.pid!=0x0C ||
        v.responder_count>kMaxResponders ||
        static_cast<uint8_t>(v.origin)>=static_cast<uint8_t>(RpmOrigin::Count) ||
        static_cast<uint8_t>(v.outcome)>static_cast<uint8_t>(RpmOutcome::TransportError) ||
        (v.acquired_us && v.acquired_us>v.observed_us))
      return false;
    // Reply-order-independent normalization, no heap.
    for (size_t i=1;i<v.responder_count;++i) {
      const RpmResponder x=v.responders[i];
      size_t j=i;
      while (j && v.responders[j-1].can_id>x.can_id) {
        v.responders[j]=v.responders[j-1];
        --j;
      }
      v.responders[j]=x;
    }
    for (size_t i=1;i<v.responder_count;++i)
      if (v.responders[i-1].can_id==v.responders[i].can_id) return false;
    if (v.outcome==RpmOutcome::Ok && !v.responder_count) return false;
    return true;
  }
  static bool sameResponseSet(const RpmRun& a,const RpmObservation& b) {
    if (a.request_can_id!=b.request_can_id ||
        a.service!=b.service || a.pid!=b.pid || a.grouped!=b.grouped ||
        a.outcome!=b.outcome || a.responder_count!=b.responder_count)
      return false;
    for (size_t i=0;i<b.responder_count;++i) {
      const auto& x=a.responders[i];
      const auto& y=b.responders[i];
      if (x.can_id!=y.can_id || x.quarter_rpm!=y.quarter_rpm ||
          x.valid!=y.valid) return false;
    }
    return true;
  }
  RpmRun runs_[kRuns]{};
  Stats stats_{};
  uint64_t prev_physical_id_=0;
  size_t head_=0,count_=0;
  bool gap_since_last_=false;
};
static_assert(sizeof(RpmRunCompressor)<=24*1024,
              "RPM RAM collector exceeds provisional 24KiB budget");
} // namespace m5can
