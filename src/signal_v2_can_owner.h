#pragma once
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
#include "can_monitor.h"
#include "ecu_physical_selector.h"
#include "signal_v2_can_budget.h"
#include "signal_v2_can_samples.h"
#include "signal_v2_can_route.h"
#include "signal_v2_simulator.h"
#include <Arduino.h>
#include <array>
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
  void stop(const char* reason,uint8_t quality=10){
    samples_.invalidate(quality);
    if(!stopped_)Serial.printf("#SIGNALV2_CAN,stopped=1,reason=%s,invalid=%lu\n",
      reason,static_cast<unsigned long>(samples_.invalidations));
    if(!stopped_){++samples_.source_generation;++samples_.mode_epoch;}
    stopped_=true;
    if(can_)can_->revokeLease();
  }
  void result(const m5can::DiagnosticResult& r){
    if(!physical_){
      m5can::DiagnosticRawFrame pair[2]{};
      ecu_.observe(r,false,pair); // #56 RAW-backed bounded selection
      if(ecu_.fixed()){
        unsigned viable=0;
        for(unsigned i=0;i<ecu_.candidates();++i){
          const auto &c=ecu_.candidate(i);
          if(!c.disqualified&&c.successes)++viable;
        }
        if(viable!=1){stop("Rejected(Ambiguous)",6);return;}
        pinned_=EcuRoute::fromResponse(ecu_.chosenResponse(),true);
        if(!pinned_.enabledForThisTrial() ||
           pinned_.request_id!=ecu_.physicalId()){
          stop("WRONG_CAN_ADDRESSING",6);return;
        }
        samples_.source_id=pinned_.response_id;
        Serial.printf("#SIGNALV2_ECU,physical_response=%08lX,probes=%u\n",
          static_cast<unsigned long>(samples_.source_id),
          unsigned(ecu_.probes()));
      }else if(ecu_.legacy())stop("NO_VERIFIED_ECU",6);
      return;
    }
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
  const CanSamples &samples()const{return samples_;}
  bool stopped()const{return stopped_;}
  bool busy()const{return in_flight_;}
  void revokeAuthorization(){if(can_)can_->revokeLease();}
  void invalidate(uint8_t reason=10){samples_.invalidate(reason);if(can_)can_->revokeLease();}
  void tick(uint32_t now,const Simulator::Demand &demand,bool armed,uint32_t epoch=0){
    if(!can_)return;
    // Optional edge-derived epoch preserves callers outside the GATT boundary.
    if(!epoch)epoch=epoch_+(armed!=previously_armed_);
    if(epoch!=epoch_){
      epoch_=epoch;
      invalidate(10);
      ++samples_.source_generation; ++samples_.mode_epoch;
      samples_.source_id=0;
      ecu_=m5can::EcuPhysicalSelector{};pinned_=EcuRoute{};
      if(armed)stopped_=false;
      had_demand_=false;next_=now;
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
    if(in_flight_||int32_t(now-next_)<0)return;
    next_=now+period_; // No backlog catch-up burst.
    const bool discovery=ecu_.probing();
    if(!discovery&&!ecu_.fixed()){stop("ECU_PIN_LOST",6);return;}
    m5can::DiagnosticRequest req{};
    req.signal_v2=true;
    req.timeout_ms=120;
    essential_=false;
    if(discovery){
      req.can_id=0x18DB33F1U;req.length=3;
      req.data[0]=1;req.data[1]=0x0C;req.data[2]=0x0D;
    }else{
      req.can_id=ecu_.physicalId();req.length=6;
      const uint8_t p[6]={1,0x0D,0x0C,0x05,0x5B,0x9A};
      for(unsigned i=0;i<6;++i)req.data[i]=p[i];
    }
    // Physical: request + <=10 candidate RX + <=1 FC, prepaid.
    // Functional: reserve ALL possible responders, never just stored ones.
    if(!budget_.reserve(now,discovery?CanBudget::DiscoveryFrames:12,120)){
      if(discovery){stop("LOAD_BUDGET_DISCOVERY");return;}
      Serial.println("#SIGNALV2_LOAD,over_budget_attempt=1,"
                     "action=SHED_AUX,priority=SPEED_RPM");
      essential_=true;req.length=3;
      req.data[0]=1;req.data[1]=0x0C;req.data[2]=0x0D;
      if(!budget_.reserve(now,4,120)){stop("LOAD_BUDGET_ESSENTIAL");return;}
    }
    if(!can_->acquireLease(550)||!can_->submitQuery(req)){
      samples_.invalidate(10);return;
    }
    physical_=!discovery;
    submitted_epoch_=epoch_;
    in_flight_=true;
  }
};
} // namespace signal_v2
#endif
