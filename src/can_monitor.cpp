#if defined(M5CAN_SIGNAL_V2_EXPERIMENTAL) && !defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL) && (!defined(M5CAN_CAN_TX_HARD_DISABLED) || !M5CAN_CAN_TX_HARD_DISABLED)
#error "Signal V2 requires CAN TX hard disabled"
#endif
#if defined(M5CAN_CAN_TX_HARD_DISABLED) && (defined(M5CAN_VEHICLE_TRIAL) || defined(M5CAN_CAN_TX_ENABLED))
#error "Conflicting CAN TX build flags"
#endif
#include "diagnostic_error_catalog.h"
#include "can_monitor.h"
#include "adaptive_multi_pid.h"

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <esp_timer.h>

#include "app_config.h"
#include "diagnostic_transaction.h"

namespace m5can {
namespace {

void clearResult(DiagnosticResult& result) {
  static_assert(std::is_trivially_copyable<DiagnosticResult>::value,
                "result reset requires plain bounded data");
  std::memset(static_cast<void*>(&result),0,sizeof(result));
  result.status=DiagnosticStatus::NoData;
}

struct QueryActiveReset {
  QueryActiveReset(CanStats* stats_in, portMUX_TYPE* mux_in)
      : stats(stats_in), mux(mux_in) {}
  CanStats* stats = nullptr;
  portMUX_TYPE* mux = nullptr;
  ~QueryActiveReset() {
    if (!stats || !mux) return;
    portENTER_CRITICAL(mux);
    stats->query_active = false;
    portEXIT_CRITICAL(mux);
  }
};

}  // namespace

void CanMonitor::setError(const char* message) {
  std::strncpy(last_error_, message ? message : "unknown",
               sizeof(last_error_) - 1);
  last_error_[sizeof(last_error_) - 1] = '\0';
}

bool CanMonitor::installDriver(twai_mode_t mode) {
#if defined(M5CAN_CAN_TX_HARD_DISABLED)
  (void)mode;return false;
#else
  twai_general_config_t general =
      TWAI_GENERAL_CONFIG_DEFAULT(cfg::kCanTxPin, cfg::kCanRxPin, mode);
  general.tx_queue_len =
      mode == TWAI_MODE_NORMAL
          ? cfg::kTwaiNormalTxQueueLen
          : cfg::kTwaiListenTxQueueLen;
  general.rx_queue_len = cfg::kTwaiRxQueueLen;
  general.alerts_enabled = TWAI_ALERT_RX_QUEUE_FULL |
                           TWAI_ALERT_BUS_OFF |
                           TWAI_ALERT_BUS_RECOVERED |
                           TWAI_ALERT_ERR_PASS |
                           TWAI_ALERT_ABOVE_ERR_WARN |
                           TWAI_ALERT_BELOW_ERR_WARN;

  const twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
  const twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if (twai_driver_install(&general, &timing, &filter) != ESP_OK) return false;
  if (twai_start() != ESP_OK) {
    twai_driver_uninstall();
    return false;
  }
  return true;
#endif
}

bool CanMonitor::begin() {
#if defined(M5CAN_CAN_TX_HARD_DISABLED)
  setError("CAN TX HARD DISABLED; TWAI NOT INITIALIZED");return true;
#endif
  if (running_) return true;
  tx_gate_ = xSemaphoreCreateMutex();
  if (!tx_gate_) {
    setError("CAN TX mutex allocation failed");
    return false;
  }
  frame_queue_ = xQueueCreate(cfg::kFrameQueueLen, sizeof(CapturedFrame));
  query_queue_ = xQueueCreate(1, sizeof(DiagnosticRequest));
  result_queue_ = xQueueCreate(1, sizeof(DiagnosticResult));
  if (!frame_queue_ || !query_queue_ || !result_queue_) {
    setError("queue allocation failed");
    return false;
  }

  if (!installDriver(TWAI_MODE_LISTEN_ONLY)) {
    setError("listen-only driver init failed");
    return false;
  }

  running_ = true;
  if (xTaskCreatePinnedToCore(rxTaskThunk,
                              "m5can-owner",
                              7168,
                              this,
                              configMAX_PRIORITIES - 3,
                              &rx_task_,
                              0) != pdPASS) {
    running_ = false;
    twai_stop();
    twai_driver_uninstall();
    setError("CAN owner task creation failed");
    return false;
  }

  setError("OK");
  return true;
}

void CanMonitor::beginObservationBlind(uint64_t at_us) {
  portENTER_CRITICAL(&stats_mux_);
  if (audit_.active && !audit_.blind_open) {
    audit_.blind_open = true;
    audit_.blind_open_since_us = at_us;
    if (!audit_.blind_count) audit_.blind_start_us = at_us;
    ++audit_.blind_count;
  }
  portEXIT_CRITICAL(&stats_mux_);
}

void CanMonitor::endObservationBlind(uint64_t at_us) {
  portENTER_CRITICAL(&stats_mux_);
  if (audit_.active && audit_.blind_open) {
    audit_.blind_open = false;
    if (at_us >= audit_.blind_open_since_us)
      audit_.blind_total_us += at_us - audit_.blind_open_since_us;
    if (audit_.blind_count == 1) audit_.blind_end_us = at_us;
  }
  portEXIT_CRITICAL(&stats_mux_);
}

bool CanMonitor::switchDriverMode(twai_mode_t mode) {
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
  const bool blind_return = mode == TWAI_MODE_LISTEN_ONLY && probe_guard_;
  if (blind_return)
    beginObservationBlind(static_cast<uint64_t>(esp_timer_get_time()));
#endif
  const esp_err_t stopped = twai_stop();
  if (stopped != ESP_OK && stopped != ESP_ERR_INVALID_STATE) return false;
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
  // The query queue can win over idle RX. Do not erase queued late probe
  // evidence on reinstall. With TWAI stopped this drain is bounded.
  if(probe_guard_) {
    bool drained=false;
    for(uint32_t i=0;i<=cfg::kTwaiRxQueueLen;++i) {
      twai_message_t pending{};
      if(twai_receive(&pending,0)!=ESP_OK){drained=true;break;}
      captureMessage(pending,static_cast<uint64_t>(esp_timer_get_time()));
    }
    if(!drained)enterFaultLocked("V2 probe RX drain incomplete");
  }
  updateDriverStats(); // Preserve loss evidence before counters reset.
#endif
  if (twai_driver_uninstall() != ESP_OK) return false;

  if (!installDriver(mode)) {
    if (mode != TWAI_MODE_LISTEN_ONLY) {
      installDriver(TWAI_MODE_LISTEN_ONLY);
    }
    return false;
  }

  updateDriverStats();
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
  if (blind_return)
    endObservationBlind(static_cast<uint64_t>(esp_timer_get_time()));
#endif
  return true;
}

void CanMonitor::endObservationAudit(ObservationAudit& copy, CanStats& final_stats) {
  audit_close_done_.store(false,std::memory_order_release);
  audit_close_requested_.store(true,std::memory_order_release);
  for (unsigned i=0;i<100 &&
       !audit_close_done_.load(std::memory_order_acquire);++i)
    vTaskDelay(pdMS_TO_TICKS(1));
  portENTER_CRITICAL(&stats_mux_);
  if (!audit_close_done_.load(std::memory_order_acquire)) {
    audit_.owner_confirmed=false;
    audit_.active=false;
  }
  audit_.rx_inflight_at_end=rx_processing_;
  copy=audit_;
  statsLocked(final_stats);
  audit_close_requested_.store(false,std::memory_order_release);
  portEXIT_CRITICAL(&stats_mux_);
}

void CanMonitor::processAuditCloseOnOwner() {
  if (!audit_close_requested_.load(std::memory_order_acquire)) return;
  const uint32_t generation=audit_generation_.load(std::memory_order_acquire);
  bool certified=true;
  uint32_t pending_seen=0;
  bool drained=false;
  for (uint32_t i=0;i<=cfg::kTwaiRxQueueLen;++i) {
    twai_status_info_t state{};
    if (twai_get_status_info(&state)!=ESP_OK) {certified=false;break;}
    twai_message_t frame{};
    if (twai_receive(&frame,0)==ESP_OK) {
      ++pending_seen;
      captureMessage(frame,static_cast<uint64_t>(esp_timer_get_time()));
      continue;
    }
    if (state.msgs_to_rx) {certified=false;pending_seen+=state.msgs_to_rx;}
    else drained=true;
    break;
  }
  twai_status_info_t last{};
  if (!drained || twai_get_status_info(&last)!=ESP_OK || last.msgs_to_rx)
    certified=false;
  // Refresh missed/overrun/bus fault evidence at the actual owner barrier,
  // not at the last periodic status poll.
  updateDriverStats();
  portENTER_CRITICAL(&stats_mux_);
  if (audit_close_requested_.load(std::memory_order_acquire) &&
      audit_generation_.load(std::memory_order_acquire)==generation) {
    audit_.owner_confirmed=certified && !pending_seen && !rx_processing_;
    audit_.pending_rx_at_close=pending_seen+last.msgs_to_rx;
    audit_.active=false;
    audit_close_done_.store(true,std::memory_order_release);
  }
  portEXIT_CRITICAL(&stats_mux_);
}

void CanMonitor::rxTaskThunk(void* arg) {
  static_cast<CanMonitor*>(arg)->rxTask();
}

CapturedFrame CanMonitor::captureMessage(const twai_message_t& message,
                                         uint64_t timestamp_us) {
  CapturedFrame frame{};
  frame.timestamp_us = timestamp_us;
  frame.identifier = message.identifier;
  frame.dlc = message.data_length_code > 8 ? 8 : message.data_length_code;
  frame.extended = message.extd;
  frame.rtr = message.rtr;
  frame.self = message.self;
  if (!frame.rtr && frame.dlc) {
    std::memcpy(frame.data, message.data, frame.dlc);
  }

  portENTER_CRITICAL(&stats_mux_);
  ++rx_processing_; // Paired below after audit_.note(), including closed windows.
  ++stats_.rx_frames;
  if (frame.extended) ++stats_.ext_frames;
  else ++stats_.std_frames;
  if (frame.rtr) ++stats_.rtr_frames;
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
  if(!frame.extended && !frame.rtr && frame.identifier>=0x7E8 &&
      frame.identifier<=0x7EF)++stats_.std_obd_responses;
#endif
  portEXIT_CRITICAL(&stats_mux_);

#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
  if(frame.extended &&
     DiagnosticPolicy::allowedResponseId(frame.identifier)) {
    portENTER_CRITICAL(&stats_mux_);++stats_.signal_v2_rx;portEXIT_CRITICAL(&stats_mux_);
    // Includes delayed replies after timeout/STOP, not just accepted PDUs.
    // External faulty senders cannot be silenced by this controller: latch
    // out our TX as soon as observed traffic cannot fit the rolling cap.
    if(!recordObservedDiagnostic(static_cast<uint32_t>(timestamp_us/1000)))
      enterFaultLocked("V2 observed diagnostic RX cap/capacity exceeded");
  }
  const bool diagnostic=DiagnosticPolicy::allowedResponseId(frame.identifier) ||
      (!frame.extended && frame.identifier>=0x7E8 && frame.identifier<=0x7EF);
  if(probe_guard_ && diagnostic &&
     (!probe_receiving_ || timestamp_us>probe_tx_us_+120000ULL))
    enterFaultLocked("V2 late/idle discovery response");
#endif
  // Independent from GUI/USB frame_queue_ consumption. The CAN owner is
  // always the single producer, including inside active ISO-TP transactions.
  RawCanFrame raw{};
  raw.timestamp_us=frame.timestamp_us;
  raw.identifier=frame.identifier;
  raw.dlc=frame.dlc;
  raw.flags=static_cast<uint8_t>((frame.extended?1U:0U) |
      (frame.rtr?2U:0U) | (frame.self?4U:0U));
  std::memcpy(raw.data,frame.data,sizeof(raw.data));
  raw.direction=RawCanDirection::Rx;
  raw_capture_.push(raw);

  if (xQueueSend(frame_queue_, &frame, 0) != pdTRUE) {
    portENTER_CRITICAL(&stats_mux_);
    ++stats_.app_queue_drops;
    portEXIT_CRITICAL(&stats_mux_);
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
    enterFaultLocked("V2 RX queue loss");
#endif
  }
  // S1 audit last (review M-07): a frame enters the observation window only
  // after all of its counters (candidate RX, drops) are updated, so an
  // audit snapshot never contains a frame whose loss is not yet counted.
  portENTER_CRITICAL(&stats_mux_);
  audit_.note(frame.identifier, frame.extended, frame.rtr, frame.dlc,
              frame.data,frame.timestamp_us);
  --rx_processing_;
  const bool audit_overflowed = audit_.overflowed_now;
  audit_.overflowed_now = false;
  portEXIT_CRITICAL(&stats_mux_);
  // An unaccounted RX ID ends the S1 evidence: latch TX off at the lower
  // gate so no lease can be re-acquired (reviews M-04/B-05).
  if (audit_overflowed) enterFaultLocked("S1 observation audit overflow");
  return frame;
}

void CanMonitor::rxTask() {
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  // Actual runtime affinity, rather than assuming the requested pin worked.
  Serial.printf("#CORE,task=can-owner,core=%d,priority=%u\n",
                xPortGetCoreID(),static_cast<unsigned>(uxTaskPriorityGet(nullptr)));
#endif
  while (running_) {
    if(audit_close_requested_.load(std::memory_order_acquire))
      processAuditCloseOnOwner();
    DiagnosticRequest request{};
    if (xQueueReceive(query_queue_, &request, 0) == pdTRUE) {
      DiagnosticResult& result=owner_result_;
      clearResult(result);
      // Both BLE sessions must share one CAN owner. A second identical
      // read-only PID/DID request can reuse a validated <=100ms result,
      // avoiding another CAN TX and expensive TWAI mode transition.
      const bool from_cache = !fault_locked_ && !request.signal_v2 &&
          result_cache_.lookup(
              request, static_cast<uint64_t>(esp_timer_get_time()), result);
      if (!from_cache) {
        performQuery(request,result);
        DiagnosticPolicy::observeFunctionalPairs(request,result);
        if(!result.completed_at_us)
          result.completed_at_us=static_cast<uint64_t>(esp_timer_get_time());
        if(!request.signal_v2)
          result_cache_.remember(request,result,result.completed_at_us);
      }
      // A negative ISO-TP response (7F <service> <NRC>) is still a
      // completed transport transaction, but is NOT a usable meter value.
      // Count only a matching *positive* PID/DID response as OBD OK.
      uint32_t positive_ecu=0;
      if (request.can_id==0x18DB33F1U && request.length==3 &&
          request.data[0]==0x01 && request.data[1]==0x0C &&
          request.data[2]==0x0D) {
        // Physical CAN receive metric: count a complete positive pair
        // from any valid responder. CPU1's coordinator separately records
        // GroupEnabled/GroupFallback after multi-ECU provenance checks.
        // These are intentionally distinct from application acceptance.
        AdaptiveMultiPid::hasCompletePositivePair(result,positive_ecu);
      } else for (uint8_t i=0;i<result.response_count;++i) {
        const DiagnosticMessage &message=result.responses[i];
        const bool positive_obd=(request.data[0]==0x01 ||
                                 request.data[0]==0x09) &&
              message.length>=2 &&
              message.data[0]==static_cast<uint8_t>(request.data[0]+0x40) &&
              message.data[1]==request.data[1];
        const bool positive_uds=request.data[0]==0x22 &&
              message.length>=3 && message.data[0]==0x62 &&
              message.data[1]==request.data[1] &&
              message.data[2]==request.data[2];
        if (positive_obd || positive_uds) {
          positive_ecu=message.can_id;
          break;
        }
      }
      portENTER_CRITICAL(&stats_mux_);
      const unsigned status_index=static_cast<unsigned>(result.status);
      if(status_index<10 && stats_.result_status_count[status_index]!=UINT32_MAX)
        ++stats_.result_status_count[status_index];
      if(result.status!=DiagnosticStatus::Ok || positive_ecu==0) {
        stats_.last_failed_request=diagnosticRequestIdentity(request);
        stats_.last_failed_ms=millis();
      }
      for(unsigned i=0;i<result.response_count && i<kDiagnosticMaxResponses;++i) {
        const auto& message=result.responses[i];
        if(message.length<3 || message.data[0]!=0x7F ||
           message.data[1]!=request.data[0])continue;
        const uint32_t detail=(static_cast<uint32_t>(message.data[1])<<24) |
            (static_cast<uint32_t>(message.data[2])<<16) | (message.can_id&0xFFFFU);
        if(!stats_.nrc_count) {
          stats_.first_nrc_detail=detail;stats_.first_nrc_ms=millis();
          stats_.first_nrc_request=diagnosticRequestIdentity(request);
        }
        if(stats_.nrc_count!=UINT32_MAX)++stats_.nrc_count;
        stats_.last_nrc_detail=detail;stats_.last_nrc_ms=millis();
        stats_.last_nrc_request=diagnosticRequestIdentity(request);
      }
      if (from_cache) ++stats_.query_cache_hits;
      if (result.status==DiagnosticStatus::Ok && positive_ecu!=0) {
        ++stats_.query_ok;
        stats_.last_response_can_id=positive_ecu;
      } else if (result.status==DiagnosticStatus::NoData) {
        ++stats_.query_no_data;
      } else {
        ++stats_.query_failed;
      }
      portEXIT_CRITICAL(&stats_mux_);
      xQueueSend(result_queue_, &result, 0);
      continue;
    }

    twai_message_t message{};
    const esp_err_t receive_result =
        twai_receive(&message, pdMS_TO_TICKS(cfg::kTwaiReceiveWaitMs));
    const uint64_t now_us =
        static_cast<uint64_t>(esp_timer_get_time());
    if (receive_result == ESP_OK) captureMessage(message, now_us);
    maybeUpdateDriverStats(now_us);
    if (receive_result != ESP_OK) taskYIELD();
  }

  rx_task_ = nullptr;
  vTaskDelete(nullptr);
}

void CanMonitor::maybeUpdateDriverStats(uint64_t now_us) {
  if (now_us - last_status_poll_us_ < 100000U) return;
  last_status_poll_us_ = now_us;
  updateDriverStats();
}

void CanMonitor::updateDriverStats() {
  twai_status_info_t info{};
  if (twai_get_status_info(&info) != ESP_OK) {
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
    enterFaultLocked("V2 driver counters unavailable");
#endif
    return;
  }
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
  if(info.rx_missed_count || info.rx_overrun_count || info.bus_error_count ||
     info.state==TWAI_STATE_BUS_OFF)
    enterFaultLocked("V2 driver RX loss/bus fault");
#endif

  portENTER_CRITICAL(&stats_mux_);
  stats_.driver_rx_missed = info.rx_missed_count;
  stats_.driver_rx_overrun = info.rx_overrun_count;
  stats_.driver_bus_error = info.bus_error_count;
  stats_.driver_arb_lost = info.arb_lost_count;
  stats_.rx_queue_depth = info.msgs_to_rx;
  stats_.state = info.state;
  portEXIT_CRITICAL(&stats_mux_);

  if (info.state == TWAI_STATE_BUS_OFF) revokeLease();
}

bool CanMonitor::acquireLease(uint32_t duration_ms) {
#if defined(M5CAN_CAN_TX_HARD_DISABLED)
  (void)duration_ms;return false;
#endif
  if (tx_gate_ && xSemaphoreTake(tx_gate_, portMAX_DELAY) != pdTRUE)
    return false;
  if (fault_locked_
#if defined(M5CAN_SIGNAL_V2_STAGE_GATED)
      || !stage_authorized_ || stage_epoch_ <= revoked_stage_epoch_
      || stage_s1_consumed_
#endif
      ) {
    if (tx_gate_) xSemaphoreGive(tx_gate_);
    return false;
  }
  if (duration_ms == 0) duration_ms = cfg::kTxLeaseDefaultMs;
  duration_ms = std::min(duration_ms, cfg::kTxLeaseMaxMs);
  const uint64_t now_ms =
      static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
  portENTER_CRITICAL(&lease_mux_);
  lease_deadline_ms_ = now_ms + duration_ms;
  portEXIT_CRITICAL(&lease_mux_);
  if (tx_gate_) xSemaphoreGive(tx_gate_);
  return true;
}

void CanMonitor::revokeLease() {
  // STOP and actual driver submission linearize on a FreeRTOS task mutex.
  // Never invoke a blocking driver API under a spinlock.
  if (tx_gate_ && xSemaphoreTake(tx_gate_, portMAX_DELAY) != pdTRUE) {
    fault_locked_ = true;
    return;
  }
  portENTER_CRITICAL(&lease_mux_);
  lease_deadline_ms_ = 0;
  portEXIT_CRITICAL(&lease_mux_);
  if (tx_gate_) xSemaphoreGive(tx_gate_);
}

bool CanMonitor::beginStageS1(uint32_t epoch) {
  if (!tx_gate_ || xSemaphoreTake(tx_gate_, portMAX_DELAY) != pdTRUE)
    return false;
  const bool accepted = epoch && epoch > revoked_stage_epoch_ &&
      epoch > stage_epoch_ && !fault_locked_;
  if (accepted) {
    stage_epoch_ = epoch;
    stage_authorized_ = true;
    stage_s1_consumed_ = false;
    stage_submit_us_ = 0;
  }
  xSemaphoreGive(tx_gate_);
  return accepted;
}

void CanMonitor::revokeStageAuthorization(uint32_t epoch) {
  if (!tx_gate_ || xSemaphoreTake(tx_gate_, portMAX_DELAY) != pdTRUE) {
    fault_locked_ = true;
    return;
  }
  if (epoch > revoked_stage_epoch_) revoked_stage_epoch_ = epoch;
  stage_authorized_ = false;
  portENTER_CRITICAL(&lease_mux_);
  lease_deadline_ms_ = 0;
  portEXIT_CRITICAL(&lease_mux_);
  xSemaphoreGive(tx_gate_);
}

uint32_t CanMonitor::leaseRemainingMs() const {
  const uint64_t now_ms =
      static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
  uint64_t deadline = 0;
  portENTER_CRITICAL(&lease_mux_);
  deadline = lease_deadline_ms_;
  portEXIT_CRITICAL(&lease_mux_);
  if (deadline <= now_ms) return 0;
  const uint64_t remaining = deadline - now_ms;
  return remaining > 0xFFFFFFFFULL
             ? 0xFFFFFFFFU
             : static_cast<uint32_t>(remaining);
}

bool CanMonitor::leaseAllowsBudget(uint32_t budget_ms) const {
  return leaseRemainingMs() >= budget_ms;
}

bool CanMonitor::waitForRateLimit(const DiagnosticRequest& request) {
  // Throttle repeated *parameter* reads only, not other PIDs with the same
  // arbitration ID. Every ECU transaction remains strictly serialized.
  while (diagnostic_pacer_.remainingUs(
             request,
             static_cast<uint64_t>(esp_timer_get_time())) > 0) {
    if (leaseRemainingMs() == 0) return false;
    twai_message_t message{};
    if (twai_receive(&message, pdMS_TO_TICKS(2)) == ESP_OK) {
      captureMessage(message,
                     static_cast<uint64_t>(esp_timer_get_time()));
    }
  }
  return true;
}

void CanMonitor::enterFaultLocked(const char* reason) {
  fault_locked_ = true;
  revokeLease();
  setError(reason);
}

esp_err_t CanMonitor::transmitOwnedFrame(const twai_message_t& message,
                                         uint32_t wait_ms) {
#if defined(M5CAN_CAN_MODE_DRY_RUN) || defined(M5CAN_CAN_TX_HARD_DISABLED)
  // USB-only mode-switch stack regression: no call to the physical TX API.
  (void)message;
  (void)wait_ms;
  return ESP_FAIL;
#else
  // Preflight is not an authorization: STOP can intervene before the gate.
  if(!leaseAllowsBudget(wait_ms))return ESP_FAIL;
  if (tx_gate_ && xSemaphoreTake(tx_gate_, portMAX_DELAY) != pdTRUE)
    return ESP_FAIL;
  // B-03: final lease/fault check and TWAI enqueue share the STOP mutex.
  // If STOP acquired it first, no new frame reaches the driver.
  if (fault_locked_ || !leaseAllowsBudget(wait_ms)
#if defined(M5CAN_SIGNAL_V2_STAGE_GATED)
      || !stage_authorized_ || stage_epoch_ <= revoked_stage_epoch_
      || stage_s1_consumed_
#endif
      ) {
    if (tx_gate_) xSemaphoreGive(tx_gate_);
    return ESP_FAIL;
  }
#if defined(M5CAN_SIGNAL_V2_STAGE_GATED)
  // Sole epoch right: physical Mode01 0C/0D; never FC or group.
  const bool valid_s1=message.extd && !message.rtr && !message.self &&
      message.ss &&
      message.identifier==0x18DA01F1U && message.data_length_code==8 &&
      message.data[0]==3 && message.data[1]==1 &&
      message.data[2]==0x0C && message.data[3]==0x0D &&
      !message.data[4] && !message.data[5] &&
      !message.data[6] && !message.data[7];
  if (!valid_s1 || stage_s1_consumed_) {
    if (tx_gate_) xSemaphoreGive(tx_gate_);
    return ESP_FAIL;
  }
  stage_s1_consumed_=true;
  // M-11: the sole S1 TX right is consumed before any pre-TX RX drain.
  // Acquire/check and driver submission remain on the STOP tx_gate_.
  // Never call the complete RX accounting path while holding that gate:
  // RX faults must be allowed to revoke the lease without recursion.
  uint32_t pre_drain_count=0;
  bool queue_drained=false;
  for (uint32_t i=0;i<cfg::kTwaiRxQueueLen;++i) {
    twai_message_t pending{};
    const esp_err_t rc=twai_receive(&pending,0);
    if (rc==ESP_ERR_TIMEOUT) {queue_drained=true;break;}
    if (rc!=ESP_OK) break;
    ++pre_drain_count;
    xSemaphoreGive(tx_gate_);
    captureMessage(pending,static_cast<uint64_t>(esp_timer_get_time()));
    if (xSemaphoreTake(tx_gate_,portMAX_DELAY)!=pdTRUE) return ESP_FAIL;
    if (fault_locked_ || !stage_authorized_ ||
        stage_epoch_<=revoked_stage_epoch_ || !leaseAllowsBudget(wait_ms)) {
      xSemaphoreGive(tx_gate_);
      return ESP_FAIL;
    }
  }
  twai_status_info_t pre_status{};
  const bool pre_status_ok=twai_get_status_info(&pre_status)==ESP_OK;
  const uint64_t pre_drain_end_us=static_cast<uint64_t>(esp_timer_get_time());
  const bool pre_empty=queue_drained && pre_status_ok && !pre_status.msgs_to_rx;
  portENTER_CRITICAL(&stats_mux_);
  if(audit_.active) {
    audit_.pre_tx_drain_frames=pre_drain_count;
    audit_.pre_tx_drain_end_us=pre_drain_end_us;
    audit_.pre_tx_queue_empty=pre_empty;
  }
  portEXIT_CRITICAL(&stats_mux_);
  if (!pre_empty || fault_locked_ || !stage_authorized_ ||
      stage_epoch_<=revoked_stage_epoch_ || !leaseAllowsBudget(wait_ms)) {
    xSemaphoreGive(tx_gate_);
    return ESP_FAIL;
  }
  // A pre-submit gap over 500us fails before the driver can accept TX.
  const uint64_t pre_submit_us=static_cast<uint64_t>(esp_timer_get_time());
  if (pre_submit_us<pre_drain_end_us ||
      pre_submit_us-pre_drain_end_us>500ULL) {
    xSemaphoreGive(tx_gate_);
    return ESP_FAIL;
  }
#endif
  portENTER_CRITICAL(&stats_mux_);
  ++stats_.tx_attempts;
  portEXIT_CRITICAL(&stats_mux_);
  const esp_err_t result =
      twai_transmit(&message, pdMS_TO_TICKS(wait_ms));
#if defined(M5CAN_SIGNAL_V2_STAGE_GATED)
  if(result==ESP_OK) {
    stage_submit_us_=static_cast<uint64_t>(esp_timer_get_time());
    const uint64_t gap=stage_submit_us_>=pre_drain_end_us ?
        stage_submit_us_-pre_drain_end_us : UINT64_MAX;
    portENTER_CRITICAL(&stats_mux_);
    if(audit_.active) {
      audit_.pre_tx_gap_us=gap;
      audit_.pre_tx_gap_ok=gap<=500ULL;
    }
    portEXIT_CRITICAL(&stats_mux_);
  }
#endif
  if (tx_gate_) xSemaphoreGive(tx_gate_);
  // A TX result is one software submission event, not a physical wire
  // timestamp. Cache responses and dry-run TX are never fabricated as TX.
  RawCanFrame raw{};
  raw.timestamp_us=static_cast<uint64_t>(esp_timer_get_time());
  raw.identifier=message.identifier;
  raw.dlc=message.data_length_code>8?8:message.data_length_code;
  raw.flags=static_cast<uint8_t>((message.extd?1U:0U) |
      (message.rtr?2U:0U) | (message.self?4U:0U));
  if(!message.rtr)std::memcpy(raw.data,message.data,raw.dlc);
  raw.direction=result==ESP_OK?RawCanDirection::TxSubmitted:
                                    RawCanDirection::TxSubmitFailed;
  raw_capture_.push(raw);
  if (result == ESP_OK) {
    portENTER_CRITICAL(&stats_mux_);
    ++stats_.tx_success;
    portEXIT_CRITICAL(&stats_mux_);
  }
  return result;
#endif
}

twai_message_t CanMonitor::makeDiagnosticRequestFrame(
    const DiagnosticRequest& request) {
  twai_message_t tx{};
  tx.identifier = request.can_id;
  tx.extd = 1;
  tx.rtr = 0;
  tx.ss = 1;
  tx.data_length_code = 8;
  tx.data[0] = request.length;
  std::memcpy(&tx.data[1], request.data, request.length);
  return tx;
}

twai_message_t CanMonitor::makeFlowControlFrame(uint32_t can_id) {
  twai_message_t tx{};
  tx.identifier = can_id;
  tx.extd = 1;
  tx.rtr = 0;
  tx.ss = 1;
  tx.data_length_code = 8;
  tx.data[0] = 0x30;
  tx.data[1] = 0x00;
  tx.data[2] = 0x00;
  return tx;
}

void CanMonitor::performQuery(
    const DiagnosticRequest& request, DiagnosticResult& result) {

  portENTER_CRITICAL(&stats_mux_);
  stats_.query_active = true;
  ++stats_.query_count;
  portEXIT_CRITICAL(&stats_mux_);
  QueryActiveReset query_active_reset{&stats_, &stats_mux_};

  const uint64_t transaction_start_us =
      static_cast<uint64_t>(esp_timer_get_time());

#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
  updateDriverStats(); // Check before driver reinstall can reset loss counters.
#endif
  if (fault_locked_) {
    result.status = DiagnosticStatus::ModeError;
    return;
  }
  if (!DiagnosticPolicy::allowedReadRequest(request)) {
    result.status = DiagnosticStatus::InvalidRequest;
    return;
  }

  const bool discovery=request.signal_v2 && request.length==3 &&
                       !request.signal_v2_essential;
  const uint32_t response_timeout_ms =
      std::max<uint32_t>(
          20, std::min<uint32_t>(request.timeout_ms,
              request.signal_v2 && request.length==3?120:1000));
  const bool allow_response_pending = request.data[0] == 0x22;
  const uint32_t budget_ms =
      DiagnosticPolicy::transactionBudgetMs(
          response_timeout_ms, allow_response_pending);

  if (!leaseAllowsBudget(budget_ms)) {
    result.status = DiagnosticStatus::LeaseRequired;
    return;
  }
  if (!waitForRateLimit(request) || !leaseAllowsBudget(budget_ms)) {
    result.status = DiagnosticStatus::LeaseRequired;
    return;
  }

  if (!switchDriverMode(TWAI_MODE_NORMAL)) {
    enterFaultLocked("enter normal mode failed");
    result.status = DiagnosticStatus::ModeError;
    return;
  }

  DiagnosticStatus terminal = DiagnosticStatus::NoData;
  bool terminal_set = false;

  const uint32_t max_window_ms =
      allow_response_pending
          ? std::max(response_timeout_ms,
                     cfg::kMaxResponsePendingWindowMs)
          : response_timeout_ms;

  if (!leaseAllowsBudget(max_window_ms + 50)) {
    terminal = DiagnosticStatus::LeaseRequired;
    terminal_set = true;
  }

  uint64_t tx_us = 0;
  uint64_t submitted_us = 0; // Driver-accepted submission time (0 = none).
  // N1: V2 never transmits a functional broadcast, whatever its budget.
  if(!terminal_set && request.signal_v2 && request.can_id==0x18DB33F1U){
    terminal=DiagnosticStatus::RateLimited;terminal_set=true;
  }
  if(!terminal_set && request.signal_v2) {
    const uint64_t now_us=static_cast<uint64_t>(esp_timer_get_time());
    if(fault_locked_ || (v2_start_seen_ && (last_v2_discovery_ || discovery) &&
        now_us-last_v2_start_us_<200000ULL)) {
      terminal=DiagnosticStatus::RateLimited;terminal_set=true;
    }
  }
  if(!terminal_set && request.signal_v2){
    const uint16_t frames=request.can_id==0x18DB33F1U
        ? signal_v2::CanBudget::DiscoveryFrames : (request.length==3?4:12);
    portENTER_CRITICAL(&stats_mux_);
    const bool observed_admitted=observed_budget_.permits(millis(),frames);
    portEXIT_CRITICAL(&stats_mux_);
    // Charge the full expected RX + FC at the real request boundary; mode
    // switching latency must not age away its future response reservation.
    if(!observed_admitted || !signal_budget_.reserve(millis(),frames,response_timeout_ms)){
      terminal=DiagnosticStatus::RateLimited;terminal_set=true;
    }
  }
  if (!terminal_set) {
    const twai_message_t tx =
        makeDiagnosticRequestFrame(request);
    tx_us = static_cast<uint64_t>(esp_timer_get_time());
    if(request.signal_v2) {
      v2_start_seen_=true;last_v2_start_us_=tx_us;last_v2_discovery_=discovery;
      probe_guard_=probe_receiving_=request.length==3;
      if(probe_guard_)probe_tx_us_=tx_us;
    }
    if(request.signal_v2 && !recordObservedDiagnostic(millis())) {
      terminal=DiagnosticStatus::RateLimited;terminal_set=true;revokeLease();
    }
    if (!terminal_set && transmitOwnedFrame(tx, 20) != ESP_OK) {
      terminal = DiagnosticStatus::DriverError;
      terminal_set = true;
    } else if (!terminal_set) {
      diagnostic_pacer_.noteSuccessfulTx(request, tx_us);
      // Taken after the driver accepted the frame (twai_transmit may wait
      // up to 20 ms), so an observation window never starts before it.
#if defined(M5CAN_SIGNAL_V2_STAGE_GATED)
      submitted_us=stage_submit_us_;
#else
      submitted_us=static_cast<uint64_t>(esp_timer_get_time());
#endif
      if (request.signal_v2 && request.length==3)
        setObservationWindow(submitted_us);
    }
  }

  DiagnosticTransaction& transaction=owner_transaction_;
  // Reset even when TX failed: reused scratch must never retain an older
  // transaction's positive response or raw frames on the error path.
  transaction.begin(
      request, tx_us,
      static_cast<uint32_t>(cfg::kIsoTpCfTimeoutUs / 1000ULL));

  uint64_t deadline_us =
      tx_us + static_cast<uint64_t>(response_timeout_ms) * 1000ULL;
  const uint64_t hard_deadline_us =
      tx_us + static_cast<uint64_t>(max_window_ms) * 1000ULL;
  uint64_t quiet_deadline_us = 0;

  while (!terminal_set) {
    const uint64_t now_us =
        static_cast<uint64_t>(esp_timer_get_time());

    if (transaction.expireIsoTp(now_us) > 0) {
      terminal = DiagnosticStatus::IsoTpError;
      terminal_set = true;
      break;
    }

    uint64_t effective_deadline_us = deadline_us;
    if (quiet_deadline_us && !transaction.hasActiveIsoTp()) {
      effective_deadline_us =
          std::min(effective_deadline_us, quiet_deadline_us);
    }
    if (now_us >= effective_deadline_us) break;

    const uint32_t remaining_ms =
        static_cast<uint32_t>(
            (effective_deadline_us - now_us + 999ULL) / 1000ULL);
    twai_message_t rx{};
    const esp_err_t receive_result =
        twai_receive(&rx,
                     pdMS_TO_TICKS(
                         std::min<uint32_t>(5, remaining_ms)));

    if (receive_result != ESP_OK) {
      updateDriverStats();
      const CanStats snapshot = stats();
      if (fault_locked_ || snapshot.state == TWAI_STATE_BUS_OFF) {
        terminal = DiagnosticStatus::BusOff;
        terminal_set = true;
        revokeLease();
      }
      continue;
    }

    const CapturedFrame captured =
        captureMessage(
            rx, static_cast<uint64_t>(esp_timer_get_time()));

    TransactionFrame frame{};
    frame.can_id = captured.identifier;
    frame.extended = captured.extended;
    frame.rtr = captured.rtr;
    frame.dlc = rx.data_length_code; // Preserve malformed DLC for contract validation.
    frame.timestamp_us = captured.timestamp_us;
    std::memcpy(frame.data, captured.data, captured.dlc);

    if(request.signal_v2 && fault_locked_) {
      terminal=DiagnosticStatus::IsoTpError;terminal_set=true;break;
    }
    const TransactionStep step = transaction.onFrame(frame);

    if (step.event == TransactionEvent::NeedFlowControl) {
      if (!leaseAllowsBudget(50)) {
        terminal = DiagnosticStatus::LeaseRequired;
        terminal_set = true;
        break;
      }

      if(request.signal_v2 &&
         (!recordObservedDiagnostic(millis()) || fault_locked_)) {
        terminal=DiagnosticStatus::RateLimited;terminal_set=true;revokeLease();break;
      }
      const twai_message_t fc =
          makeFlowControlFrame(step.flow_control_can_id);
      if (transmitOwnedFrame(fc, 20) != ESP_OK) {
        terminal = DiagnosticStatus::DriverError;
        terminal_set = true;
        break;
      }
      if(request.signal_v2){
        portENTER_CRITICAL(&stats_mux_);++stats_.signal_v2_fc;portEXIT_CRITICAL(&stats_mux_);
      }
      quiet_deadline_us = 0;
    } else if (step.event == TransactionEvent::Completed) {
      if (!transaction.hasActiveIsoTp() && !discovery) {
        quiet_deadline_us =
            captured.timestamp_us + cfg::kPostResponseQuietUs;
      }
    } else if (step.event == TransactionEvent::ResponsePending) {
      if (!allow_response_pending ||
          !leaseAllowsBudget(
              cfg::kResponsePendingExtensionMs + 50)) {
        terminal = DiagnosticStatus::LeaseRequired;
        terminal_set = true;
        break;
      }

      deadline_us =
          std::min<uint64_t>(
              hard_deadline_us,
              captured.timestamp_us +
                  static_cast<uint64_t>(
                      cfg::kResponsePendingExtensionMs) *
                      1000ULL);
      quiet_deadline_us = 0;
    } else if (step.event == TransactionEvent::ProtocolError) {
      terminal = DiagnosticStatus::IsoTpError;
      terminal_set = true;
      break;
    }
  }

  if (!terminal_set) {
    result = transaction.result();
    if (result.response_count) {
      terminal = DiagnosticStatus::Ok;
    } else if (transaction.hasActiveIsoTp()) {
      terminal = DiagnosticStatus::IsoTpError;
    } else {
      terminal = DiagnosticStatus::NoData;
      portENTER_CRITICAL(&stats_mux_);
      ++stats_.query_timeout;
      portEXIT_CRITICAL(&stats_mux_);
    }
  } else if (tx_us) {
    result = transaction.result();
  }

  // The transaction result copies above replace the struct; restore the
  // submission time on every path that transmitted.
  result.request_tx_us = submitted_us;
  probe_receiving_=false; // Keep idle guard until another admitted TX.
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
  updateDriverStats();
  if(request.signal_v2 && (terminal==DiagnosticStatus::IsoTpError || fault_locked_)) {
    enterFaultLocked("V2 transaction contract/accounting fault");
    terminal=DiagnosticStatus::IsoTpError;
  }
#endif
  result.status = terminal;

  if (!switchDriverMode(TWAI_MODE_LISTEN_ONLY)) {
    enterFaultLocked("return listen-only failed");
    result.status = DiagnosticStatus::ModeError;
  }

  result.transaction_us =
      static_cast<uint32_t>(
          static_cast<uint64_t>(esp_timer_get_time()) -
          transaction_start_us);
  return;
}

bool CanMonitor::submitQuery(const DiagnosticRequest& request) {
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
  if(!request.signal_v2)return false; // USB/V1 BLE cannot transmit in this image.
#endif
  if (!query_queue_ || !result_queue_) return false;
#if defined(M5CAN_SIGNAL_V2_STAGE_GATED)
  const bool valid_s1=request.signal_v2 && request.can_id==0x18DA01F1U &&
      request.length==3 && request.data[0]==1 && request.data[1]==0x0C &&
      request.data[2]==0x0D && !request.signal_v2_essential;
  if (!valid_s1 || !tx_gate_ || xSemaphoreTake(tx_gate_,portMAX_DELAY)!=pdTRUE)
    return false;
  const bool stage_allows=stage_authorized_ && !stage_s1_consumed_ &&
      stage_epoch_>revoked_stage_epoch_ && !fault_locked_;
  xSemaphoreGive(tx_gate_);
  if (!stage_allows) return false;
#endif
  // Reserve the single in-flight slot atomically across USB and BLE callers.
  // IMPORTANT: never call FreeRTOS queues while holding an ESP spinlock.
  portENTER_CRITICAL(&query_mux_);
  const bool accepted = !fault_locked_ && !query_in_flight_ && !log_checkpoint_active_;
  if (accepted) query_in_flight_ = true;
  portEXIT_CRITICAL(&query_mux_);
  if (!accepted) return false;

  DiagnosticResult stale{};
  while (xQueueReceive(result_queue_, &stale, 0) == pdTRUE) {}
  if (xQueueSend(query_queue_, &request, 0) == pdTRUE) return true;

  portENTER_CRITICAL(&query_mux_);
  query_in_flight_ = false;
  portEXIT_CRITICAL(&query_mux_);
  return false;
}

bool CanMonitor::pollQueryResult(DiagnosticResult& result) {
  if (!query_in_flight_ || !result_queue_) return false;
  // A query is owned by the caller that successfully submitted it. The two
  // DualBleElm sessions dispatch through one owner and the USB path submits
  // only when that owner is idle. Do not block under the query spinlock.
  if (xQueueReceive(result_queue_, &result, 0) != pdTRUE) return false;
  portENTER_CRITICAL(&query_mux_);
  query_in_flight_ = false;
  portEXIT_CRITICAL(&query_mux_);
  return true;
}

bool CanMonitor::query(const DiagnosticRequest& request,
                       DiagnosticResult& result,
                       TickType_t wait_ticks) {
  if (!submitQuery(request)) return false;
  if (xQueueReceive(result_queue_, &result, wait_ticks) != pdTRUE) {
    return false;
  }
  portENTER_CRITICAL(&query_mux_);
  query_in_flight_ = false;
  portEXIT_CRITICAL(&query_mux_);
  return true;
}

bool CanMonitor::pop(CapturedFrame& frame, TickType_t wait_ticks) {
  return frame_queue_ &&
         xQueueReceive(frame_queue_, &frame, wait_ticks) == pdTRUE;
}

bool CanMonitor::recordObservedDiagnostic(uint32_t now) {
  portENTER_CRITICAL(&stats_mux_);
  const bool ok=observed_budget_.observe(now);
  portEXIT_CRITICAL(&stats_mux_);
  return ok;
}

CanStats CanMonitor::stats() const {
  CanStats copy{};
  stats(copy);
  return copy;
}

void CanMonitor::stats(CanStats& copy) const {
  portENTER_CRITICAL(&stats_mux_);
  statsLocked(copy);
  portEXIT_CRITICAL(&stats_mux_);
}


}  // namespace m5can
