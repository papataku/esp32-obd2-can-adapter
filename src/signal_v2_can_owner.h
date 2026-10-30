#pragma once
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
#include "can_monitor.h"
#include "ecu_physical_selector.h"
#include "signal_v2_can_budget.h"
#include "signal_v2_can_samples.h"
#include "signal_v2_can_route.h"
#include "signal_v2_discovery_profile.h"
#include "signal_v2_simulator.h"
#include <Arduino.h>
#include <array>
#include <atomic>
#include <cstring>
namespace signal_v2 {
// Runs only in BLE application owner task, never inside a NimBLE callback.
// CAN owner task remains the sole transmitter through CanMonitor::submitQuery.
class CanOwner {
  friend struct CanOwnerTestAccess; // Host fixture supplies RAW-verified discovery results.
  m5can::CanMonitor* can_=nullptr;
  m5can::EcuPhysicalSelector ecu_{};
  CanBudget budget_{};
  CanSamples samples_{};
  EcuRoute pinned_{}; // Protocol-aware ECU identity; 11bit future use.
  bool in_flight_=false,physical_=false,essential_=false,stopped_=false;
  bool previously_armed_=false,had_demand_=false;
  uint32_t next_=0,last_log_=0,epoch_=0,submitted_epoch_=0;
  uint64_t prior_11bit_=0,prior_bus_errors_=0;
  uint16_t period_=200;
  // N1 bounded physical discovery state; reset at every epoch.
  static constexpr uint32_t kDiscoveryDeadlineMs=3000;
  std::array<uint32_t,DiscoveryProfile::kMaxCandidates> verified_{};
  uint32_t probe_target_=0,discovery_started_=0;
  uint8_t probe_index_=0,verified_count_=0;
  bool discovery_running_=false;
  // PR-C2 stage S1 (owner decision O-1): an explicitly requested, separate
  // authorization. One physical 0C/0D request, 1,000 ms receive/count window,
  // automatic STOP. Never pins an ECU, never publishes a value.
  static constexpr uint32_t kS1ObservationMs=1000;
  bool s1_=false,s1_sent_=false,s1_reported_=false;
  // A stage request is bound to the exact authorization epoch it created
  // (review F1); any later epoch (disconnect, STOP) cannot consume it.
  std::atomic<uint32_t> s1_requested_epoch_{0};
  // Why the authorization ended, set by the GATT side before it bumps the
  // epoch (review F3). Atomic: disconnect runs on the NimBLE task.
  std::atomic<const char*> s1_end_reason_{nullptr};
  uint32_t s1_started_=0,s1_enqueued_at_=0,s1_tx_ms_=0;
  const char* s1_outcome_="NOT_SENT";
  m5can::CanStats s1_base_{};
  void stop(const char* reason,uint8_t quality=10){
    samples_.invalidate(quality);
    if(!stopped_)Serial.printf("#SIGNALV2_CAN,stopped=1,reason=%s,invalid=%lu\n",
      reason,static_cast<unsigned long>(samples_.invalidations));
    if(!stopped_){++samples_.source_generation;++samples_.mode_epoch;}
    stopped_=true;
    if(can_){
#if defined(M5CAN_SIGNAL_V2_STAGE_GATED)
      can_->revokeStageAuthorization(epoch_);
#else
      can_->revokeLease();
#endif
    }
    if(s1_)reportS1(reason);
  }
  // One parseable record per S1 epoch. complete=0 rejects the evidence.
  void reportS1(const char* reason){
    if(s1_reported_||!can_)return;
    s1_reported_=true;
    m5can::ObservationAudit audit{};
    m5can::CanStats s{};
    can_->endObservationAudit(audit,s); // Same boundary as the per-ID counts.
    const auto d=[](uint64_t now,uint64_t base){return now>=base?now-base:0ULL;};
    // M-07: reject closure during a partially processed RX, or a window
    // where the global RX counter and ID audit span different frames.
    const bool rx_consistent=!audit.rx_inflight_at_end &&
      s.rx_frames>=s1_base_.rx_frames &&
      s.rx_frames-s1_base_.rx_frames==static_cast<uint64_t>(audit.accounted_rx)
      && audit.window_set;
    const bool lossless=rx_consistent && audit.owner_confirmed &&
      !audit.pending_rx_at_close && !audit.blind_open && !audit.overflow &&
      s.driver_rx_missed==s1_base_.driver_rx_missed &&
      s.driver_rx_overrun==s1_base_.driver_rx_overrun && s.app_queue_drops==s1_base_.app_queue_drops &&
      s.driver_bus_error==s1_base_.driver_bus_error;
    const bool complete=lossless && s1_tx_ms_ && !std::strcmp(reason,"S1_OBSERVATION_COMPLETE");
    Serial.printf("#SIGNALV2_S1,epoch=%lu,start_ms=%lu,enqueue_ms=%lu,request_ms=%lu,end_ms=%lu,requests=%u,"
      "fc=%llu,all_rx=%lu,candidate_rx=%llu,tx_attempts=%llu,tx_success=%llu,"
      "rx_missed=%lu,rx_overrun=%lu,queue_drops=%llu,bus_error=%lu,"
      "reserved_peak100=%lu,reserved_peak1000=%lu,charged_peak100=%lu,charged_peak1000=%lu,"
      "id_overflow=%lu,rx_counted=%llu,rx_inflight_end=%lu,outside_pre=%lu,outside_post=%lu,window_start_us=%llu,owner_confirmed=%u,rx_pending_end=%lu,blind_count=%lu,blind_start_us=%llu,blind_end_us=%llu,blind_total_us=%llu,blind_open=%u,outcome=%s,stop=%s,complete=%u\n",
      (unsigned long)epoch_,(unsigned long)s1_started_,(unsigned long)s1_enqueued_at_,(unsigned long)s1_tx_ms_,
      (unsigned long)millis(),s1_sent_?1U:0U,
      (unsigned long long)d(s.signal_v2_fc,s1_base_.signal_v2_fc),(unsigned long)audit.all_rx,
      (unsigned long long)d(s.signal_v2_rx,s1_base_.signal_v2_rx),
      (unsigned long long)d(s.tx_attempts,s1_base_.tx_attempts),
      (unsigned long long)d(s.tx_success,s1_base_.tx_success),
      (unsigned long)(s.driver_rx_missed-s1_base_.driver_rx_missed),
      (unsigned long)(s.driver_rx_overrun-s1_base_.driver_rx_overrun),
      (unsigned long long)d(s.app_queue_drops,s1_base_.app_queue_drops),
      (unsigned long)(s.driver_bus_error-s1_base_.driver_bus_error),
      (unsigned long)budget_.peak100,(unsigned long)budget_.peak1000,
      (unsigned long)s.observed_peak100,(unsigned long)s.observed_peak1000,
      (unsigned long)audit.overflow,
      (unsigned long long)d(s.rx_frames,s1_base_.rx_frames),
      (unsigned long)audit.rx_inflight_at_end,
      (unsigned long)audit.outside_before,
      (unsigned long)audit.outside_after,
      (unsigned long long)audit.window_start_us,
      audit.owner_confirmed?1U:0U,
      (unsigned long)audit.pending_rx_at_close,
      (unsigned long)audit.blind_count,
      (unsigned long long)audit.blind_start_us,
      (unsigned long long)audit.blind_end_us,
      (unsigned long long)audit.blind_total_us,
      audit.blind_open?1U:0U,
      s1_outcome_,reason,complete?1U:0U);
    for(unsigned i=0;i<audit.used;++i)
      Serial.printf("#SIGNALV2_S1_ID,epoch=%lu,id=%08lX,ext=%u,sf=%lu,ff=%lu,cf=%lu,fc=%lu,other=%lu\n",
        (unsigned long)epoch_,(unsigned long)audit.ids[i],unsigned(audit.extended[i]),
        (unsigned long)audit.counts[i][0],(unsigned long)audit.counts[i][1],
        (unsigned long)audit.counts[i][2],(unsigned long)audit.counts[i][3],
        (unsigned long)audit.counts[i][4]);
  }
  void s1Tick(uint32_t now){
    // Any frame beyond the fixed per-ID capacity is unaccounted evidence.
    if(can_->observationAuditOverflowed()){stop("S1_AUDIT_OVERFLOW",10);return;}
    // (The monitor itself also revokes the lease the moment the audit
    // overflows, so a request already queued cannot be transmitted; M-04.)
    if(s1_sent_){
      if(in_flight_){
        // A transaction ends 120 ms after TX; no completion within the
        // window means the observation cannot be bounded (review F2).
        if(uint32_t(now-s1_enqueued_at_)>=kS1ObservationMs)stop("S1_NO_COMPLETION",10);
        return;
      }
      // The window is measured from the actual driver submission time.
      if(int32_t(now-s1_tx_ms_)>=int32_t(kS1ObservationMs))
        stop("S1_OBSERVATION_COMPLETE",6);
      return;
    }
    if(in_flight_)return; // A previous epoch's completion is discarded first.
    if(DiscoveryProfile::kCount!=1){stop("NO_APPROVED_CANDIDATE",6);return;}
    const EcuRoute route=DiscoveryProfile::kCandidates[0].route();
    if(!route.enabledForThisTrial()){stop("WRONG_CAN_ADDRESSING",6);return;}
    if(!budget_.reserve(now,CanBudget::ProbeFrames,120)){stop("LOAD_BUDGET_DISCOVERY");return;}
    m5can::DiagnosticRequest req{};
    req.signal_v2=true;req.timeout_ms=120;
    req.can_id=route.request_id;req.length=3;
    req.data[0]=1;req.data[1]=0x0C;req.data[2]=0x0D;
    probe_target_=route.response_id;
    if(!can_->acquireLease(550)||!can_->submitQuery(req)){
      stop("QUERY_ADMISSION_FAILED",10);return;
    }
    s1_sent_=true;s1_enqueued_at_=now;
    discovery_running_=true;discovery_started_=now;
    physical_=false;essential_=false;submitted_epoch_=epoch_;in_flight_=true;
  }
  void pin(uint32_t response_id){
    if(!ecu_.pinBoundedPhysical(response_id)){stop("WRONG_CAN_ADDRESSING",6);return;}
    pinned_=EcuRoute::fromResponse(ecu_.chosenResponse(),true);
    if(!pinned_.enabledForThisTrial() ||
       pinned_.request_id!=ecu_.physicalId()){
      stop("WRONG_CAN_ADDRESSING",6);return;
    }
    samples_.source_id=pinned_.response_id;
    Serial.printf("#SIGNALV2_ECU,physical_response=%08lX,probes=%u\n",
      static_cast<unsigned long>(samples_.source_id),unsigned(probe_index_));
  }
  // Exactly the probed candidate, Single Frames only, each RAW-backed, and
  // 0C plus 0D once each (one combined PDU or two per-PID PDUs).
  static bool validProbe(const m5can::DiagnosticResult& r,uint32_t id){
    if(r.status!=m5can::DiagnosticStatus::Ok || !r.completed_at_us ||
       r.response_count<1 || r.response_count>2 ||
       r.raw_frame_count!=r.response_count)return false;
    for(unsigned j=0;j<r.raw_frame_count;++j)
      if(r.raw_frames[j].can_id!=id || r.raw_rx_offsets_us[j]>120000U)return false;
    bool rpm=false,speed=false;
    for(unsigned i=0;i<r.response_count;++i){
      const auto &m=r.responses[i];
      if(m.can_id!=id || m.length<3 || m.length>7 || m.data[0]!=0x41)return false;
      bool raw=false;
      for(unsigned j=0;j<r.raw_frame_count;++j){
        const auto &f=r.raw_frames[j];
        if(f.dlc>=m.length+1U && f.data[0]==m.length &&
           std::memcmp(&f.data[1],m.data,m.length)==0)raw=true;
      }
      if(!raw)return false;
      for(unsigned pos=1;pos<m.length;){
        if(m.data[pos]==0x0C && !rpm && pos+3U<=m.length){rpm=true;pos+=3;}
        else if(m.data[pos]==0x0D && !speed && pos+2U<=m.length){speed=true;pos+=2;}
        else return false;
      }
    }
    return rpm && speed;
  }
  // No response only means "not this candidate". Anything else that is not
  // the exact contract (FF/CF, extra, duplicate, negative, wrong source,
  // driver/budget error) latches STOP; never assemble a partial success.
  void probeResult(const m5can::DiagnosticResult& r){
    if(s1_){
      if(!r.request_tx_us){s1_outcome_="NOT_TRANSMITTED";stop("S1_NOT_TRANSMITTED",10);return;}
      s1_tx_ms_=uint32_t(r.request_tx_us/1000ULL);
      if(r.status==m5can::DiagnosticStatus::NoData &&
         !r.response_count && !r.raw_frame_count)s1_outcome_="NO_RESPONSE";
      else if(validProbe(r,probe_target_))s1_outcome_="SINGLE_FRAME_CONTRACT";
      else{s1_outcome_="CONTRACT_VIOLATION";stop("DISCOVERY_RESPONSE_CONTRACT",6);}
      return; // Observation continues; S1 never pins or requests again.
    }
    if(r.status==m5can::DiagnosticStatus::NoData &&
       !r.response_count && !r.raw_frame_count){
    }else if(validProbe(r,probe_target_)){
      verified_[verified_count_++]=probe_target_;
    }else{stop("DISCOVERY_RESPONSE_CONTRACT",6);return;}
    if(++probe_index_<DiscoveryProfile::kCount)return;
    if(verified_count_==0){stop("NO_VERIFIED_ECU",6);return;}
    if(verified_count_!=1){stop("Rejected(Ambiguous)",6);return;}
    pin(verified_[0]);
  }
  void result(const m5can::DiagnosticResult& r){
    if(!physical_){probeResult(r);return;}
    std::array<CanSamples::Sample,5> parsed{};
    bool valid=false;
    if(r.status==m5can::DiagnosticStatus::Ok && r.completed_at_us &&
       r.response_count>=1 && ecu_.fixed() &&
       pinned_.enabledForThisTrial() &&
       pinned_.request_id==ecu_.physicalId()){
      if(r.response_count==1 && r.responses[0].can_id==ecu_.chosenResponse()){
        valid=pinned_.matches(EcuRoute::fromResponse(r.responses[0].can_id,true)) &&
          CanSamples::decodeGroup(r.responses[0].data,
                         r.responses[0].length,!essential_,parsed);
      }
      // Some ECUs return one positive response per PID, not one combined PDU.
      // Accept exactly five (or essential two), same ECU, no duplicates,
      // no missing/extra/negative records. Otherwise fail closed atomically.
      const unsigned expected=essential_?2U:5U;
      if(!valid && r.response_count==expected){
        std::array<bool,5> seen{};
        valid=true;
        for(unsigned k=0;k<r.response_count;++k){
          const auto &m=r.responses[k];
          bool matched=false;
          for(unsigned j=0;j<expected;++j){
            const uint8_t len=CanSamples::Lengths[j];
            if(!seen[j] && pinned_.matches(
                   EcuRoute::fromResponse(m.can_id,true)) &&
               m.length==len && m.data[0]==0x41 &&
               m.data[1]==CanSamples::Pids[j]){
              seen[j]=true;matched=true;
              parsed[j].length=len;
              for(unsigned b=0;b<len;++b)parsed[j].raw[b]=m.data[b];
              break;
            }
          }
          if(!matched){valid=false;break;}
        }
        for(unsigned j=0;j<expected;++j)if(!seen[j])valid=false;
      }
    }
    ecu_.observeSingle(valid,r.status);
    if(!valid){
      samples_.invalidate(r.status==m5can::DiagnosticStatus::NoData?3:5);
      Serial.printf("#SIGNALV2_CAN,quality=INVALID,reason=RESPONSE,"
                    "status=%u,invalid=%lu\n",unsigned(r.status),
                    static_cast<unsigned long>(samples_.invalidations));
      if(!ecu_.fixed())stop("ECU_PIN_LOST",6);
      return;
    }
    samples_.source_id=pinned_.response_id;
    for(unsigned i=0;i<5;++i){
      auto &s=samples_.values[i];
      if(essential_&&i>=2){
        s.length=0;s.quality=8;s.observed_us=0;continue;
      }
      s.length=parsed[i].length;
      for(unsigned j=0;j<s.length;++j)s.raw[j]=parsed[i].raw[j];
      s.quality=0; // Numeric zero is valid only when actually decoded.
      s.observed_us=r.completed_at_us;
      ++s.sequence;
    }
    ++samples_.acquired;
  }
public:
  void attach(m5can::CanMonitor* can){can_=can;}
  // Called with the authorization epoch increment of `SIGV2 CAN S1` only.
  void requestStageS1(uint32_t epoch){s1_requested_epoch_.store(epoch);}
  // Record why authorization is ending before the caller bumps the epoch.
  void noteAuthorizationEnd(const char* reason){
    const char* none=nullptr;
    s1_end_reason_.compare_exchange_strong(none,reason); // keep the first cause (N-01)
  }
  bool stageS1()const{return s1_;}
  const CanSamples &samples()const{return samples_;}
  bool stopped()const{return stopped_;}
  bool busy()const{return in_flight_;}
  void revokeAuthorization(uint32_t revoked_epoch=0){
    if(!can_)return;
#if defined(M5CAN_SIGNAL_V2_STAGE_GATED)
    if(revoked_epoch){can_->revokeStageAuthorization(revoked_epoch);return;}
#else
    (void)revoked_epoch;
#endif
    can_->revokeLease();
  }
  void invalidate(uint8_t reason=10){samples_.invalidate(reason);if(can_)can_->revokeLease();}
  void tick(uint32_t now,const Simulator::Demand &demand,bool armed,uint32_t epoch=0){
    if(!can_)return;
    // Optional edge-derived epoch preserves callers outside the GATT boundary.
    if(!epoch)epoch=epoch_+(armed!=previously_armed_);
    if(epoch!=epoch_){
      // An interrupted S1 (operator STOP, expiry, disconnect) emits its record
      // with its own epoch and cause before the state is replaced (F3).
      if(s1_&&!s1_reported_){
        const char* why=s1_end_reason_.load();
        reportS1(why?why:"EPOCH_CHANGED");
      }else if(s1_requested_epoch_.load() && s1_requested_epoch_.load()!=epoch){
        const uint32_t pending=s1_requested_epoch_.load();
        // Granted but superseded before this task ever observed its epoch
        // (review M-05): no TX happened; record that explicitly.
        const char* why=s1_end_reason_.load();
        Serial.printf("#SIGNALV2_S1,epoch=%lu,requests=0,outcome=NOT_STARTED,stop=%s,complete=0\n",
          (unsigned long)pending,why?why:"EPOCH_CHANGED");
      }
      s1_end_reason_.store(nullptr);
      epoch_=epoch;
      invalidate(10);
      ++samples_.source_generation; ++samples_.mode_epoch;
      samples_.source_id=0;
      ecu_=m5can::EcuPhysicalSelector{};pinned_=EcuRoute{};
      verified_={};probe_target_=0;probe_index_=verified_count_=0;
      discovery_running_=false;discovery_started_=0;
      if(armed)stopped_=false;
      had_demand_=false;next_=now;
      // A stage request applies only to the epoch it created.
      s1_=armed&&s1_requested_epoch_.load()==epoch;
      s1_requested_epoch_.store(0);
      s1_sent_=s1_reported_=false;s1_outcome_="NOT_SENT";
      s1_started_=now;s1_enqueued_at_=s1_tx_ms_=0;
      if(s1_){
#if defined(M5CAN_SIGNAL_V2_STAGE_GATED)
        if(!can_->beginStageS1(epoch_)){stop("S1_AUTH_REVOKED",10);return;}
#endif
        can_->beginObservationAudit(s1_base_);
      }
    }
    if(armed && !stopped_) {
      const auto health=can_->stats();
      if(health.driver_rx_missed || health.driver_rx_overrun ||
         health.app_queue_drops || health.driver_bus_error ||
         health.state==TWAI_STATE_BUS_OFF || can_->faultLocked())
        stop("RX_ACCOUNTING_OR_BUS_FAULT",10);
      if(discovery_running_ && !ecu_.fixed() &&
         uint32_t(now-discovery_started_)>=kDiscoveryDeadlineMs)
        stop("DISCOVERY_TIMEOUT",6);
    }
    if(in_flight_){
      m5can::DiagnosticResult incoming{};
      if(can_->pollQueryResult(incoming)){
        in_flight_=false;
        if(armed&&!stopped_&&submitted_epoch_==epoch_)result(incoming);
        else samples_.invalidate(10); // Never promote disarmed completion.
      }
    }
    const auto load=can_->stats();
    budget_.refresh(now);
    if(uint32_t(now-last_log_)>=1000){
      last_log_=now;
      Serial.printf("#SIGNALV2_LOAD,reserved_window100_bits=%lu,reserved_window1000_bits=%lu,"
         "reserved_peak100=%lu,reserved_peak1000=%lu,actual_candidate_rx_frames=%llu,actual_fc_submissions=%llu,"
         "charged_traffic_window100_bits=%lu,charged_traffic_window1000_bits=%lu,charged_traffic_peak100=%lu,charged_traffic_peak1000=%lu,"
         "limit100=5000,limit1000=25000,denied=%lu,acquired=%lu,"
         "invalid=%lu,requested_period=%u\n",
         static_cast<unsigned long>(budget_.used100),
         static_cast<unsigned long>(budget_.used1000),
         static_cast<unsigned long>(budget_.peak100),
         static_cast<unsigned long>(budget_.peak1000),
         static_cast<unsigned long long>(load.signal_v2_rx),
         static_cast<unsigned long long>(load.signal_v2_fc),
         static_cast<unsigned long>(load.observed100),
         static_cast<unsigned long>(load.observed1000),
         static_cast<unsigned long>(load.observed_peak100),
         static_cast<unsigned long>(load.observed_peak1000),
         static_cast<unsigned long>(budget_.denied),
         static_cast<unsigned long>(samples_.acquired),
         static_cast<unsigned long>(samples_.invalidations),unsigned(period_));
    }
    if(!armed){
      if(previously_armed_)samples_.invalidate(10);
      previously_armed_=false;
      had_demand_=false;
      can_->revokeLease();
      return;
    }
    previously_armed_=true;
    if(stopped_)return;
    const auto s=can_->stats();
    if(s.std_obd_responses>prior_11bit_){
      stop("11BIT_OBD_RESPONSE",6);return;
    }
    prior_11bit_=s.std_obd_responses;
    if(s.driver_bus_error>prior_bus_errors_||
       s.state==TWAI_STATE_BUS_OFF||can_->faultLocked()){
      stop("TWAI_BUS_ERROR",10);return;
    }
    prior_bus_errors_=s.driver_bus_error;
    if(s1_){s1Tick(now);return;} // Independent of BLE demand; never LIVE.
#if defined(M5CAN_SIGNAL_V2_STAGE_GATED)
    // Owner decision O-1 / review B-04: in the vehicle-capable staged build the
    // owner itself refuses any non-S1 transmission, independent of how the
    // armed flag and epoch were observed by the caller.
    can_->revokeLease();
    return;
#endif
    bool has_demand=false;period_=10000;
    for(auto p:demand)if(p){
      has_demand=true;
      if(p<period_)period_=p;
    }
    if(!has_demand){
      if(had_demand_)samples_.invalidate(2);
      had_demand_=false;
      can_->revokeLease();
      return;
    }
    had_demand_=true;
#if defined(M5CAN_SIGNAL_V2_10HZ_LAB)
    if(period_<100)period_=100;
#else
    if(period_<200)period_=200;
#endif
    const bool discovery=ecu_.state()==m5can::EcuPhysicalSelector::State::Discover;
    if(!discovery&&!ecu_.fixed()){stop("ECU_PIN_LOST",6);return;}
    if(discovery&&discovery_running_&&
       uint32_t(now-discovery_started_)>=kDiscoveryDeadlineMs){
      stop("DISCOVERY_TIMEOUT",6);return;
    }
    if(in_flight_||int32_t(now-next_)<0)return;
    next_=now+(discovery && period_<200?200:period_); // Guard probe -> group too.
    m5can::DiagnosticRequest req{};
    req.signal_v2=true;
    req.timeout_ms=120;
    essential_=false;
    if(discovery){
      // Serial physical probes to approved candidates only; never functional.
      if(probe_index_>=DiscoveryProfile::kCount){
        stop(DiscoveryProfile::kCount?"NO_VERIFIED_ECU":"NO_APPROVED_CANDIDATE",6);
        return;
      }
      const EcuRoute route=DiscoveryProfile::kCandidates[probe_index_].route();
      if(!route.enabledForThisTrial()){stop("WRONG_CAN_ADDRESSING",6);return;}
      if(!discovery_running_){discovery_running_=true;discovery_started_=now;}
      probe_target_=route.response_id;
      req.can_id=route.request_id;req.length=3;
      req.data[0]=1;req.data[1]=0x0C;req.data[2]=0x0D;
    }else{
      req.can_id=ecu_.physicalId();req.length=6;
      const uint8_t p[6]={1,0x0D,0x0C,0x05,0x5B,0x9A};
      for(unsigned i=0;i<6;++i)req.data[i]=p[i];
    }
    // Physical group: request + <=10 candidate RX + <=1 FC, prepaid.
    // Discovery probe: request + <=2 SF + 1 allowance, no FC (PR-A section 2).
    // At 100ms cadence, two full 120ms reservations leave no room even
    // for the next essential read. Keep one essential reservation of
    // headroom when choosing a full group; 5Hz selection is unchanged.
    const bool full_headroom=discovery || period_>=200 || budget_.permits(now,16);
    if(!full_headroom || !budget_.reserve(now,discovery?CanBudget::ProbeFrames:12,120)){
      if(discovery){stop("LOAD_BUDGET_DISCOVERY");return;}
      // Shed only regular auxiliary reads; keep the same pinned ECU and
      // reserve the entire SF-only essential transaction before submission.
      essential_=true;req.signal_v2_essential=true;req.length=3;
      req.data[0]=1;req.data[1]=0x0C;req.data[2]=0x0D;
      if(!budget_.reserve(now,4,120)){stop("LOAD_BUDGET_ESSENTIAL");return;}
    }
    if(!can_->acquireLease(550)||!can_->submitQuery(req)){
      stop("QUERY_ADMISSION_FAILED",10);return;
    }
    physical_=!discovery;
    submitted_epoch_=epoch_;
    in_flight_=true;
  }
};
} // namespace signal_v2
#endif
