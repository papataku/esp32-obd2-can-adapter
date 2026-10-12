#include "dual_ble_elm.h"
#if defined(M5CAN_DEMO_FAULT_FIXTURE) && !defined(M5CAN_CAN_MODE_DRY_RUN)
#error "DEMO fault fixture requires physical CAN TX compiled out"
#endif
#include <esp_timer.h>
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "app_config.h"
#include "diagnostic_policy.h"
#include "diagnostic_batch_planner.h"
#include "elm_response_formatter.h"
#include "serial_protocol.h"
#include "drive_log.h"
#include "diagnostic_error_catalog.h"

namespace m5can {
namespace {
void upperNoSpaces(const char* source, char* dest, size_t capacity) {
  size_t n = 0;
  for (const char* p = source; *p && n + 1 < capacity; ++p) {
    const char c = *p;
    if (c == ' ' || c == '\t') continue;
    dest[n++] = c >= 'a' && c <= 'z' ? static_cast<char>(c-'a'+'A') : c;
  }
  dest[n] = 0;
}

}

DualBleElm::Session* DualBleElm::find(uint16_t conn_id, uint32_t generation) {
  for (auto& session : sessions_) {
    if (session.active && session.conn_id == conn_id &&
        session.generation == generation) return &session;
  }
  return nullptr;
}

void DualBleElm::event(const BleClientEvent& event) {
  if (event.type == BleClientEvent::Type::Connect) {
    // Disconnect events from older GATT generations must never affect a
    // freshly reconnected peer that reuses the same controller conn_id.
    for (auto& slot : sessions_) if (!slot.active) {
      slot = Session{};
      slot.active = true;
      slot.conn_id = event.connection_id;
      slot.generation = event.generation;
      return;
    }
    return;
  }
  Session* s = find(event.connection_id,event.generation);
  if (!s) return;
  if (event.type == BleClientEvent::Type::Disconnect) {
    // If the probing owner vanishes mid-query, its result cannot be
    // classified safely. Avoid a second speculative probe this boot.
    if (s->multi_pid_pair ||
        (active_cross_group_ &&
         ((active_query_owner_ >= 0 &&
           &sessions_[active_query_owner_] == s) ||
          (active_secondary_owner_ >= 0 &&
           &sessions_[active_secondary_owner_] == s))))
      adaptive_multi_pid_.disable();
    driveLog().event(DriveLog::Kind::Disconnect,s->conn_id);
    s->multi_pid_pair = false;
    s->active = false;
    s->pending = false;
    s->batch = false;
    s->output_size = 0;
    if (!ble_.connectedClientCount()) can_.revokeLease();
    return;
  }
  if (event.byte == '\r' || event.byte == '\n') {
    if (s->input_overflow) {
      ble_.recordDiagnosticError(s->conn_id,s->generation,
          static_cast<uint16_t>(DiagnosticError::ElmInputOverflow),
          static_cast<uint32_t>(s->input_size));
      emitLine(*s, "?");
      prompt(*s);
    } else if (s->input_size) {
      s->input[s->input_size] = 0;
      onLine(*s,s->input);
    }
    s->input_size = 0;
    s->input_overflow = false;
    return;
  }
  if (event.byte < 0x20 || event.byte > 0x7e) return;
  if (s->input_size < sizeof(s->input) - 1) {
    s->input[s->input_size++] = static_cast<char>(event.byte);
  } else s->input_overflow = true;
}

void DualBleElm::flush(Session& s) {
  if (!s.output_size) return;
  if (s.active && ble_.isClientConnected(s.conn_id,s.generation))
    ble_.queueTo(s.conn_id,s.generation,s.output,s.output_size);
  s.output_size = 0;
}

void DualBleElm::emitBytes(Session& s, const uint8_t* data, size_t length) {
  while (length) {
    if (s.output_size == sizeof(s.output)) flush(s);
    const size_t n = std::min(length,sizeof(s.output)-s.output_size);
    std::memcpy(s.output+s.output_size,data,n);
    s.output_size += n;
    data += n;
    length -= n;
  }
}
void DualBleElm::emit(Session& s, const char* text) {
  if (text && *text)
    emitBytes(s,reinterpret_cast<const uint8_t*>(text),std::strlen(text));
}
void DualBleElm::emitLine(Session& s, const char* text) {
  emit(s,text);
  static const uint8_t cr[] = {'\r'};
  static const uint8_t lf[] = {'\n'};
  emitBytes(s,cr,sizeof(cr));
  if (s.elm.linefeed()) emitBytes(s,lf,sizeof(lf));
}
void DualBleElm::prompt(Session& s) {
  static const uint8_t mark[] = {'>'};
  emitBytes(s,mark,sizeof(mark));
  flush(s);
}

void DualBleElm::appendFrame(Session& s, const DiagnosticRawFrame& frame) {
  char line[128]{};
  if (ElmResponseFormatter::formatRawFrame(
          frame,s.elm.headers(),s.elm.spaces(),line,sizeof(line)))
    emitLine(s,line);
}
void DualBleElm::appendResult(Session& s,const DiagnosticResult& result) {
  switch (result.status) {
    case DiagnosticStatus::Ok:
      if (!result.raw_frame_count) emitLine(s,"NO DATA");
      for (uint8_t i=0;i<result.raw_frame_count;++i)
        appendFrame(s,result.raw_frames[i]);
      break;
    case DiagnosticStatus::NoData: emitLine(s,"NO DATA"); break;
    case DiagnosticStatus::LeaseRequired: emitLine(s,"M5CAN TX LOCKED"); break;
    case DiagnosticStatus::InvalidRequest: emitLine(s,"?"); break;
    case DiagnosticStatus::Busy:
    case DiagnosticStatus::RateLimited: emitLine(s,"BUSY"); break;
    case DiagnosticStatus::BusOff: emitLine(s,"BUS ERROR"); break;
    default: emitLine(s,"CAN ERROR"); break;
  }
}

DiagnosticRequest DualBleElm::requestOf(const ElmVehicleRequest& item,
                                        const ElmCompat& elm) {
  DiagnosticRequest request{};
  request.can_id=item.can_id;
  request.length=item.length;
  std::memcpy(request.data,item.data,
              std::min<size_t>(item.length,sizeof(request.data)));
  const uint32_t configured=elm.timeout4ms()?elm.timeout4ms()*4U:200U;
  request.timeout_ms=std::max<uint32_t>(20,std::min<uint32_t>(1000,configured));
  return request;
}

void DualBleElm::advanceBatch(Session& s) {
  while (s.active && s.batch && s.batch_next < s.batch_count) {
    const ElmVehicleRequest& item=s.items[s.batch_next];
    // Probe ONLY in the separately opted-in grouped CAN build (never in DEMO).
    // A failed probe disables this exact functional PID pair until reboot.
    // Two BLE clients share one decision and still share one CAN owner.
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
    if (!demo_source_.demoActive() &&
        s.batch_next + 1 < s.batch_count) {
      const DiagnosticRequest first=requestOf(item,s.elm);
      const DiagnosticRequest second=
          requestOf(s.items[s.batch_next + 1],s.elm);
      // A group is a transmission optimization for already-requested
      // CACHE MISSES. If either PID is fresh, serve it independently and
      // do not issue an unnecessary grouped physical CAN query.
      const uint64_t now_us=static_cast<uint64_t>(esp_timer_get_time());
      if (!pid_cache_.hasFresh(first,now_us) &&
          !pid_cache_.hasFresh(second,now_us)) {
        const auto plan=DiagnosticBatchPlanner::plan(
            first,&second,adaptive_multi_pid_);
        if (plan.kind==DiagnosticBatchPlanner::Kind::GroupDistinct) {
          s.request=plan.transmission;
          s.multi_pid_pair=true;
          s.pending=true;
          return;
        }
      }
    }
#endif
    char label[32]{};
    if (item.length==3 && item.data[0]==0x22)
      std::snprintf(label,sizeof(label),"M5ITEM:01:%02X%02X",
                    item.data[1],item.data[2]);
    else
      std::snprintf(label,sizeof(label),"M5ITEM:00:%02X%02X",
                    item.data[0],item.data[1]);
    emitLine(s,label);
    s.request=requestOf(item,s.elm);
    if (!DiagnosticPolicy::allowedReadRequest(s.request)) {
      emitLine(s,"?");
      ++s.batch_next;
      continue;
    }
    if (demo_source_.demoActive()) {
      DiagnosticRawFrame frames[2]{};
      const uint8_t n = item.length==2 && item.data[0]==0x01
          ? DemoDrive::frames(item.data[1],millis(),frames):0;
      if (!n) emitLine(s,"NO DATA");
      for (uint8_t i=0;i<n;++i) appendFrame(s,frames[i]);
      ++s.batch_next;
      continue;
    }
    s.pending=true;
    return;
  }
  if (s.batch && s.batch_next == s.batch_count) {
    emitLine(s,"M5DONE");
    s.batch=false;
    prompt(s);
  }
}

void DualBleElm::onLine(Session& s,const char* line) {
  if (s.pending || s.batch) {
    ble_.recordDiagnosticError(s.conn_id,s.generation,
        static_cast<uint16_t>(DiagnosticError::ElmBusy));
    // Exactly one outstanding command per BLE client; the other client's
    // independent ELM session remains free to enqueue a request.
    static const uint8_t busy[]={'B','U','S','Y','\r','>'};
    ble_.queueTo(s.conn_id,s.generation,busy,sizeof(busy));
    return;
  }
  char compact[96]{};
  upperNoSpaces(line,compact,sizeof(compact));
  if (s.elm.echo()) emitLine(s,line);
  const ElmResult parsed=s.elm.execute(line);
  bool prompt_now=true;
  switch (parsed.action) {
    case ElmAction::ReplyOnly:
      if (parsed.reply[0]) {
        if(!std::strcmp(parsed.reply,"?") ||
           !std::strcmp(parsed.reply,"ERROR"))
          ble_.recordDiagnosticError(s.conn_id,s.generation,
              static_cast<uint16_t>(DiagnosticError::ElmInvalidCommand));
        if (!std::strcmp(compact,"ATM5CAP")) {
          char caps[200]{};
          std::snprintf(caps,sizeof(caps),"%s DEMO=1 MODE=%s",
                        parsed.reply,demo_source_.demoActive()?"DEMO":"CAN");
#if defined(M5CAN_DEMO_FAULT_FIXTURE)
          std::strncat(caps," FAULTS=1",sizeof(caps)-std::strlen(caps)-1);
#endif
          emitLine(s,caps);
        } else emitLine(s,parsed.reply);
      }
      break;
    case ElmAction::BatchRead:
      if (can_.faultLocked()) {
        ble_.recordDiagnosticError(s.conn_id,s.generation,
            static_cast<uint16_t>(DiagnosticError::CanModeError));
        emitLine(s,"M5CAN TX LOCKED");
      }
      else {
        s.batch_count=parsed.batch_count;
        s.batch_next=0;
        for(uint8_t i=0;i<s.batch_count;++i) s.items[i]=parsed.batch[i];
        s.batch=true;
        advanceBatch(s);
        prompt_now=false;
      }
      break;
    case ElmAction::VehicleRead:
      s.request=requestOf(parsed.request,s.elm);
      if (!DiagnosticPolicy::allowedReadRequest(s.request)) {
        ble_.recordDiagnosticError(s.conn_id,s.generation,
            static_cast<uint16_t>(DiagnosticError::CanInvalidRequest),
            s.request.can_id);
        emitLine(s,"?");
      }
      else if (demo_source_.demoActive()) {
#if defined(M5CAN_DEMO_FAULT_FIXTURE)
        // Explicit TX-disabled bench only. No CAN ownership/query changes.
        if (s.request.length==3 && s.request.data[0]==0x22) {
          const uint16_t did=(static_cast<uint16_t>(s.request.data[1])<<8)|s.request.data[2];
          if (did==0xFFFE) {
            ble_.recordDiagnosticError(s.conn_id,s.generation,
                static_cast<uint16_t>(DiagnosticError::DemoNoPrompt),diagnosticRequestIdentity(s.request,true));
            prompt_now=false;break;
          } // no bytes/prompt
          if (did==0xFFFD || did==0xFFFC) {
            ble_.recordDiagnosticError(s.conn_id,s.generation,
                static_cast<uint16_t>(DiagnosticError::DemoNrc),diagnosticRequestIdentity(s.request,true));
            emitLine(s,did==0xFFFD?"18DAF101037F2231":"18DAF101037F2233");break;
          }
          if (did==0x2012) {
            emitLine(s,"18DAF101100A622012AABBCC");
            emitLine(s,"BUFFER FULL"); break;
          }
        }
#endif
        DiagnosticRawFrame frames[2]{};
        const uint8_t n=parsed.request.length==2 && parsed.request.data[0]==0x01
            ? DemoDrive::frames(parsed.request.data[1],millis(),frames):0;
        if (!n) {
          ble_.recordDiagnosticError(s.conn_id,s.generation,
              static_cast<uint16_t>(DiagnosticError::DemoNoData),diagnosticRequestIdentity(s.request,true));
          emitLine(s,"NO DATA");
        }
        for(uint8_t i=0;i<n;++i) appendFrame(s,frames[i]);
      } else if (can_.faultLocked()) {
        ble_.recordDiagnosticError(s.conn_id,s.generation,
            static_cast<uint16_t>(DiagnosticError::CanModeError));
        emitLine(s,"M5CAN TX LOCKED");
      }
      else {
        s.pending=true;
        prompt_now=false;
      }
      break;
    case ElmAction::LeaseAcquire: {
      const bool granted=can_.acquireLease(cfg::kTxLeaseDefaultMs);
      if(!granted)ble_.recordDiagnosticError(s.conn_id,s.generation,
          static_cast<uint16_t>(DiagnosticError::CanLeaseRequired));
      emitLine(s,granted?"OK":"CAN ERROR");
      break;
    }
    case ElmAction::LeaseRevoke:
      can_.revokeLease();
      emitLine(s,"OK");
      break;
    case ElmAction::LeaseStatus: {
      char linebuf[100]{};
      std::snprintf(linebuf,sizeof(linebuf),
          "M5CAN TX=%s REM=%lums HDR=%08lX LINK=BLE",
          can_.leaseRemainingMs()?"LEASED":"LOCKED",
          static_cast<unsigned long>(can_.leaseRemainingMs()),
          static_cast<unsigned long>(s.elm.effectiveHeader()));
      emitLine(s,linebuf);
      break;
    }
    case ElmAction::ExitElmMode:
      if (parsed.reply[0]) emitLine(s,parsed.reply);
      s.elm.reset();
      break;
  }
  if (prompt_now) prompt(s);
}

void DualBleElm::finishSingle(Session& s, const DiagnosticResult& result) {
  appendResult(s,result);
  s.pending=false;
  if (s.batch) {
    ++s.batch_next;
    advanceBatch(s);
  } else prompt(s);
}

void DualBleElm::abortBatch(Session& s, const DiagnosticResult& result) {
  appendResult(s,result);
  s.pending=false;
  if (s.batch) {
    s.batch=false;
    emitLine(s,"M5DONE");
  }
  prompt(s);
}

void DualBleElm::cacheSafePartialGroup(
    const DiagnosticResult& result, const DiagnosticRequest& rpm,
    const DiagnosticRequest& speed, uint64_t timestamp_us) {
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
  // Salvage only a positive, complete, unambiguous ONE-PID response.
  // Split-ECU, stale/wrong ECU, multi-message and negative replies cannot
  // poison the shared cache. No extra physical read is manufactured.
  if (result.status!=DiagnosticStatus::Ok || result.response_count!=1 ||
      result.raw_frame_count!=1) return;
  const DiagnosticMessage& m=result.responses[0];
  if (!adaptive_multi_pid_.responderMatchesLearnedEcu(m.can_id) ||
      m.length < 3 || m.data[0]!=0x41 ||
      result.raw_frames[0].can_id!=m.can_id) return;
  if (m.data[1]==0x0C && m.length==4)
    pid_cache_.rememberSingle(rpm,result,timestamp_us);
  else if (m.data[1]==0x0D && m.length==3)
    pid_cache_.rememberSingle(speed,result,timestamp_us);
#else
  (void)result;
  (void)rpm;
  (void)speed;
  (void)timestamp_us;
#endif
}

void DualBleElm::onResult() {
  if (active_query_owner_<0) return;
  // The one CAN transaction completes on this same owner task.
  // Reuse fixed object storage rather than a large local result.
  DiagnosticResult& result=query_result_workspace_;
  if (!can_.pollQueryResult(result)) return;
  if(result.status!=DiagnosticStatus::Ok) {
    const uint16_t error=diagnosticStatusErrorCode(result.status);
    const int original=active_query_owner_;
    if(original>=0 && original<2)
      ble_.recordDiagnosticError(sessions_[original].conn_id,
           sessions_[original].generation,error,result.transaction_us);
    if(original>=0 && original<2)
      ble_.recordDiagnosticError(sessions_[original].conn_id,
          sessions_[original].generation,static_cast<uint16_t>(DiagnosticError::RequestContext),
          diagnosticRequestIdentity(sessions_[original].request));
  }
  if(active_query_owner_>=0 && active_query_owner_<2) {
    const auto& owner=sessions_[active_query_owner_];
    for(unsigned i=0;i<result.response_count && i<kDiagnosticMaxResponses;++i) {
      const auto& message=result.responses[i];
      if(message.length>=3 && message.data[0]==0x7F && message.data[1]==owner.request.data[0]) {
        ble_.recordDiagnosticError(owner.conn_id,owner.generation,
            static_cast<uint16_t>(DiagnosticError::RequestContext),diagnosticRequestIdentity(owner.request));
        ble_.recordDiagnosticError(owner.conn_id,owner.generation,
            static_cast<uint16_t>(DiagnosticError::ResponseSource),message.can_id);
      }
    }
  }
  const int index=active_query_owner_;
  const int second=active_secondary_owner_;
  const uint32_t secondary_generation=active_secondary_generation_;
  const bool cross=active_cross_group_;
  active_query_owner_=-1;
  active_secondary_owner_=-1;
  active_secondary_generation_=0;
  active_cross_group_=false;
  Session& s=sessions_[index];
  const bool primary_live=s.active &&
      s.generation==active_query_generation_;
  Session* other=second<0?nullptr:&sessions_[second];
  const bool other_live=other && other->active &&
      other->generation==secondary_generation &&
      ble_.isClientConnected(other->conn_id,other->generation);
  const uint64_t now_us=static_cast<uint64_t>(esp_timer_get_time());
  const uint64_t acquired_us =
      result.completed_at_us ? result.completed_at_us : now_us;

  if (cross) {
    // The pair was constructed solely from TWO ALREADY PENDING peers.
    // Losing one peer invalidates the speculative support decision. The
    // other peer retains its individual pending request for normal retry.
    if (!primary_live || !other_live) {
      ble_.recordDiagnosticError(s.conn_id,s.generation,
          static_cast<uint16_t>(DiagnosticError::GroupPeerGone));
      ble_.recordDiagnosticError(s.conn_id,s.generation,
          static_cast<uint16_t>(DiagnosticError::GroupPeerGone));
      adaptive_multi_pid_.disable();
      return;
    }
    DiagnosticRawFrame virtual_frames[2]{};
    uint32_t responder=0;
    const bool first_success=
        adaptive_multi_pid_.support()==AdaptiveMultiPid::Support::Unknown;
    (void)first_success; // consumed only by the grouped-profile sidecar
    auto decision=
        adaptive_multi_pid_.evaluate(result,virtual_frames,responder);
    const bool s_rpm=s.request.data[1]==0x0C;
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
    const DiagnosticRequest& rpm=s_rpm?s.request:other->request;
    const DiagnosticRequest& speed=s_rpm?other->request:s.request;
    if (decision==AdaptiveMultiPid::Decision::UseGrouped &&
        !pid_cache_.rememberApprovedPair(
            rpm,speed,virtual_frames,acquired_us)) {
      // A previously identified ECU differs from this grouped responder.
      // Do not send or cache ambiguous signals as successful readings.
      adaptive_multi_pid_.rejectCacheEcuConflict();
      decision=AdaptiveMultiPid::Decision::RetryLegacy;
    }
#endif
    if(decision!=AdaptiveMultiPid::Decision::UseGrouped)
      ble_.recordDiagnosticError(s.conn_id,s.generation,
          static_cast<uint16_t>(decision==AdaptiveMultiPid::Decision::Stop ?
              DiagnosticError::GroupStopped:DiagnosticError::GroupRejected),
          (static_cast<uint32_t>(adaptive_multi_pid_.lastRejectReason())<<24) |
          (static_cast<uint32_t>(result.status)<<16) |
          (static_cast<uint32_t>(result.response_count)<<8) |
          static_cast<uint32_t>(result.raw_frame_count));
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
    if(first_success)
      driveLog().recordFirstProbe(
          s.conn_id,result,responder,static_cast<uint8_t>(decision),
          static_cast<uint8_t>(adaptive_multi_pid_.lastRejectReason()));
    else if(decision!=AdaptiveMultiPid::Decision::UseGrouped)
      driveLog().recordFirstFailure(
          s.conn_id,result,responder,static_cast<uint8_t>(decision),
          static_cast<uint8_t>(adaptive_multi_pid_.lastRejectReason()));
#endif
    driveLog().outcome(decision==AdaptiveMultiPid::Decision::UseGrouped
        ? DriveLog::Kind::GroupEnabled
        : decision==AdaptiveMultiPid::Decision::RetryLegacy
          ? DriveLog::Kind::GroupFallback : DriveLog::Kind::GroupStopped,
        s.conn_id,result,responder);
    if (decision==AdaptiveMultiPid::Decision::UseGrouped) {
      // The per-item M5ITEM lines were already emitted when staged.
      // Never rewrite or cross-deliver a peer's batch response.
      appendFrame(s,virtual_frames[s_rpm?0:1]);
      appendFrame(*other,virtual_frames[s_rpm?1:0]);
      s.pending=false;
      other->pending=false;
      if (s.batch) {
        ++s.batch_next;
        advanceBatch(s);
      } else prompt(s);
      if (other->batch) {
        ++other->batch_next;
        advanceBatch(*other);
      } else prompt(*other);
    } else if (decision==AdaptiveMultiPid::Decision::Stop) {
      // No speculative individual CAN TX after lease, mode or bus failure.
      abortBatch(s,result);
      abortBatch(*other,result);
    } else {
      // A single complete RPM or speed returned from the group is usable
      // independently. Re-fetch ONLY the still-missing PID.
      const bool s_rpm=s.request.data[1]==0x0C;
      cacheSafePartialGroup(result,s_rpm?s.request:other->request,
                            s_rpm?other->request:s.request,acquired_us);
    }
    // RetryLegacy leaves both individual requests pending. The next
    // dispatch will observe the disabled capability and send them one by
    // one, respecting the existing CAN owner's per-PID 50ms guard.
    return;
  }

  if (!primary_live) {
    ble_.recordDiagnosticError(s.conn_id,s.generation,
        static_cast<uint16_t>(DiagnosticError::AppPeerGone),
        static_cast<uint32_t>(result.status));
    if (other_live) finishSingle(*other,result);
    return;
  }
  if (s.multi_pid_pair) {
    s.multi_pid_pair=false;
    s.pending=false;
    DiagnosticRawFrame virtual_frames[2]{};
    uint32_t responder=0;
    const bool first_success=
        adaptive_multi_pid_.support()==AdaptiveMultiPid::Support::Unknown;
    (void)first_success; // consumed only by the grouped-profile sidecar
    auto decision=
        adaptive_multi_pid_.evaluate(result,virtual_frames,responder);
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
    const DiagnosticRequest rpm=requestOf(s.items[s.batch_next],s.elm);
    const DiagnosticRequest speed=requestOf(s.items[s.batch_next+1],s.elm);
    if (decision==AdaptiveMultiPid::Decision::UseGrouped &&
        !pid_cache_.rememberApprovedPair(
            rpm,speed,virtual_frames,acquired_us)) {
      adaptive_multi_pid_.rejectCacheEcuConflict();
      decision=AdaptiveMultiPid::Decision::RetryLegacy;
    }
#endif
    if(decision!=AdaptiveMultiPid::Decision::UseGrouped)
      ble_.recordDiagnosticError(s.conn_id,s.generation,
          static_cast<uint16_t>(decision==AdaptiveMultiPid::Decision::Stop ?
              DiagnosticError::GroupStopped:DiagnosticError::GroupRejected),
          (static_cast<uint32_t>(adaptive_multi_pid_.lastRejectReason())<<24) |
          (static_cast<uint32_t>(result.status)<<16) |
          (static_cast<uint32_t>(result.response_count)<<8) |
          static_cast<uint32_t>(result.raw_frame_count));
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
    if(first_success)
      driveLog().recordFirstProbe(
          s.conn_id,result,responder,static_cast<uint8_t>(decision),
          static_cast<uint8_t>(adaptive_multi_pid_.lastRejectReason()));
    else if(decision!=AdaptiveMultiPid::Decision::UseGrouped)
      driveLog().recordFirstFailure(
          s.conn_id,result,responder,static_cast<uint8_t>(decision),
          static_cast<uint8_t>(adaptive_multi_pid_.lastRejectReason()));
#endif
    driveLog().outcome(decision==AdaptiveMultiPid::Decision::UseGrouped
        ? DriveLog::Kind::GroupEnabled
        : decision==AdaptiveMultiPid::Decision::RetryLegacy
          ? DriveLog::Kind::GroupFallback : DriveLog::Kind::GroupStopped,
        s.conn_id,result,responder);
    if (decision==AdaptiveMultiPid::Decision::UseGrouped) {
      emitLine(s,"M5ITEM:00:010C");
      appendFrame(s,virtual_frames[0]);
      emitLine(s,"M5ITEM:00:010D");
      appendFrame(s,virtual_frames[1]);
      s.batch_next=static_cast<uint8_t>(s.batch_next+2);
    } else {
      if (decision==AdaptiveMultiPid::Decision::Stop) {
        emitLine(s,"M5ITEM:00:010C");
        appendResult(s,result);
        emitLine(s,"M5ITEM:00:010D");
        appendResult(s,result);
        s.batch=false;
        emitLine(s,"M5DONE");
        prompt(s);
        return;
      }
      const DiagnosticRequest rpm=requestOf(s.items[s.batch_next],s.elm);
      const DiagnosticRequest speed=requestOf(s.items[s.batch_next+1],s.elm);
      cacheSafePartialGroup(result,rpm,speed,acquired_us);
    }
    advanceBatch(s);
    return;
  }

  // A successful single PID updates the same cache used by both peers,
  // whether it was sent alone or followed a failed group probe. Cache
  // failures invalidate ONLY the requested PID, never unrelated samples.
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
  pid_cache_.rememberSingle(s.request,result,acquired_us);
#endif
  finishSingle(s,result);
  if (other_live && other->pending) finishSingle(*other,result);
}

void DualBleElm::dispatch() {
  if (active_query_owner_>=0 || can_.queryPending()) return;
  for (int scan=0;scan<2;++scan) {
    const int i=(round_robin_+scan)%2;
    Session& s=sessions_[i];
    if (!s.active || !s.pending ||
        !ble_.isClientConnected(s.conn_id,s.generation)) continue;
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
    if (can_.faultLocked()) {
      ble_.recordDiagnosticError(s.conn_id,s.generation,
          static_cast<uint16_t>(DiagnosticError::CanModeError));
      // A cached sample is not authorization for a fresh diagnostic read.
      // Do not report fresh values while the physical bus is fault locked.
      pid_cache_.clear();
      std::memset(static_cast<void*>(&cache_workspace_),0,sizeof(cache_workspace_));
      cache_workspace_.status=DiagnosticStatus::LeaseRequired;
      if (s.multi_pid_pair) {
        s.multi_pid_pair=false;
        adaptive_multi_pid_.disable();
        emitLine(s,"M5ITEM:00:010C");
        appendResult(s,cache_workspace_);
        emitLine(s,"M5ITEM:00:010D");
        abortBatch(s,cache_workspace_);
      } else abortBatch(s,cache_workspace_);
      continue;
    }
#endif
    if (s.multi_pid_pair &&
        adaptive_multi_pid_.support()==AdaptiveMultiPid::Support::Disabled) {
      s.multi_pid_pair=false;
      s.pending=false;
      advanceBatch(s);
      if (!s.pending) continue;
    }
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
    const uint64_t now_us=static_cast<uint64_t>(esp_timer_get_time());
    if (s.multi_pid_pair) {
      const DiagnosticRequest rpm=requestOf(s.items[s.batch_next],s.elm);
      const DiagnosticRequest speed=requestOf(s.items[s.batch_next+1],s.elm);
      if (pid_cache_.hasFresh(rpm,now_us) ||
          pid_cache_.hasFresh(speed,now_us)) {
        // The second peer was staged before the first peer completed.
        // Replan the *remaining misses* rather than redundantly sending a
        // grouped CAN frame containing an already-cached PID.
        s.multi_pid_pair=false;
        s.pending=false;
        advanceBatch(s);
        if (!s.pending) continue;
      }
    }
    if (!s.multi_pid_pair) {
      if (pid_cache_.lookup(s.request,now_us,cache_workspace_)) {
        cache_hits_.fetch_add(1,std::memory_order_relaxed);
        finishSingle(s,cache_workspace_);
        // Another device may now use that same PID value.
        continue;
      }
      cache_misses_.fetch_add(1,std::memory_order_relaxed);
    }
#endif

    // Already requested, actually pending and individually missing PID
    // intents are the ONLY candidates. Planning never reads cache state.
    bool cross=false;
    DiagnosticRequest outgoing=s.request;
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
    const int j=1-i;
    Session& peer=sessions_[j];
    const bool other_pending=peer.active && peer.pending &&
        !peer.multi_pid_pair &&
        ble_.isClientConnected(peer.conn_id,peer.generation);
    const DiagnosticRequest* other=
        other_pending && !s.multi_pid_pair &&
        !pid_cache_.hasFresh(peer.request,now_us) ? &peer.request : nullptr;
    const auto plan=DiagnosticBatchPlanner::plan(
        s.request,other,adaptive_multi_pid_);
    outgoing=plan.transmission;
    cross=plan.kind==DiagnosticBatchPlanner::Kind::GroupDistinct;
    const bool shared=plan.kind==DiagnosticBatchPlanner::Kind::JoinSameParameter;
#endif

    if (!can_.acquireLease(cfg::kTxLeaseMaxMs)) {
      ble_.recordDiagnosticError(s.conn_id,s.generation,
          static_cast<uint16_t>(DiagnosticError::CanLeaseRequired),
          outgoing.can_id);
      driveLog().event(DriveLog::Kind::LeaseDenied,s.conn_id);
      s.pending=false;
      if (s.multi_pid_pair) {
        s.multi_pid_pair=false;
        adaptive_multi_pid_.disable();
        emitLine(s,"M5ITEM:00:010C");
        emitLine(s,"M5CAN TX LOCKED");
        emitLine(s,"M5ITEM:00:010D");
        emitLine(s,"M5CAN TX LOCKED");
        s.batch_next=static_cast<uint8_t>(s.batch_next+2);
        s.batch=false;
        emitLine(s,"M5DONE");
      } else {
        emitLine(s,"M5CAN TX LOCKED");
        if (s.batch) ++s.batch_next;
      }
      if (s.batch) advanceBatch(s);
      else prompt(s);
      continue;
    }
    if (can_.submitQuery(outgoing)) {
      driveLog().submitted(outgoing);
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
      if (shared) joins_.fetch_add(1,std::memory_order_relaxed);
      if (cross) cross_groups_.fetch_add(1,std::memory_order_relaxed);
#endif
      if (outgoing.length==3 && outgoing.data[0]==0x01 &&
          outgoing.data[1]==0x0C && outgoing.data[2]==0x0D)
        grouped_submissions_.fetch_add(1,std::memory_order_relaxed);
      else
        single_submissions_.fetch_add(1,std::memory_order_relaxed);
      active_query_owner_=i;
      active_query_generation_=s.generation;
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
      active_secondary_owner_=(shared || cross)?j:-1;
      active_secondary_generation_=(shared || cross)?peer.generation:0;
#else
      active_secondary_owner_=-1;
      active_secondary_generation_=0;
#endif
      active_cross_group_=cross;
      round_robin_=static_cast<uint8_t>((i+1)%2);
      return;
    }
  }
}

void DualBleElm::poll() {
  BleClientEvent event_item{};
  for(int i=0;i<512 && ble_.pollClientEvent(event_item);++i)
    event(event_item);
  onResult();
  dispatch();
  ble_.pollTx();
}
} // namespace m5can

#endif // M5CAN_DUAL_BLE_EXPERIMENTAL
