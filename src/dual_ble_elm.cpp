#include "dual_ble_elm.h"
#if defined(M5CAN_DEMO_FAULT_FIXTURE) && !defined(M5CAN_CAN_MODE_DRY_RUN)
#error "DEMO fault fixture requires physical CAN TX compiled out"
#endif
#include <esp_timer.h>
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(M5CAN_RAW_CAN_LOG_RUNTIME)
#include "raw_can_service.h"
#endif
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
#if defined(M5CAN_ECU_SELECTION_EXPERIMENTAL)
    ecu_selector_.disable();
#endif
    driveLog().event(DriveLog::Kind::Disconnect,s->conn_id);
#if defined(M5CAN_LOG_STREAM_V2_EXPERIMENTAL) && defined(M5CAN_RAW_CAN_LOG_RUNTIME)
    if(log_stream_active_ && log_stream_peer_==s->conn_id &&
       log_stream_generation_==s->generation) {
      log_stream_sender_.cancel();
      log_stream_active_=log_stream_fetch_pending_=false;
      log_stream_fetch_watchdog_.clear();
    }
#endif
#if defined(M5CAN_RAW_CAN_LOG_RUNTIME)
    // Losing the recording meter requests a clean cut-off. If power dies,
    // the immutable committed blocks remain recoverable after reboot.
    if(raw_service_)raw_service_->queueStopFromBle(s->conn_id,s->generation);
#endif
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
    // The 512-byte staging buffer is included in responseReady() reserve.
    // Keep packing adjacent small PID results into full notification chunks.
    if(!ble_.responseReady(s.conn_id,s.generation))return;
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
      if (
#if defined(M5CAN_ECU_SELECTION_EXPERIMENTAL)
          ecu_selector_.probing() ||
#endif
          (!pid_cache_.hasFresh(first,now_us) &&
           !pid_cache_.hasFresh(second,now_us))) {
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
#if defined(M5CAN_RAW_CAN_LOG_RUNTIME)
  if(!std::strncmp(compact,"ATM5LOG",7)) {
    // This ELM-compatible control plane is deliberately distinct from
    // arbitrary CAN requests. The writer acts on the next Arduino poll:
    // PENDING is NOT a durable recording ACK. No raw data is exposed here.
    if(!raw_service_)emitLine(s,"M5LOG UNAVAILABLE");
    else if(!std::strcmp(compact,"ATM5LOGCAP")) {
      emitLine(s,raw_service_->ready()?
          "M5LOG CAP V1 READY=1 READ=1 CHUNK=48 CRC=CRC32":
          "M5LOG CAP V1 READY=0 READ=0 CHUNK=48 CRC=CRC32");
    } else if(!std::strcmp(compact,"ATM5LOGSTATUS")) {
      char info[200]{};
      std::snprintf(info,sizeof(info),
          "M5LOG STATUS STATE=%u SESSION=%lu Q=%lu LOST=%lu BLOCKS=%lu BYTES=%lu STORE_ERR=%lu CTRL_ERR=%lu",
          static_cast<unsigned>(raw_service_->state()),
          static_cast<unsigned long>(raw_service_->session()),
          static_cast<unsigned long>(raw_service_->pending()),
          static_cast<unsigned long>(raw_service_->dropped()),
          static_cast<unsigned long>(raw_service_->committedBlocks()),
          static_cast<unsigned long>(raw_service_->committedBytes()),
          static_cast<unsigned long>(raw_service_->storageErrors()),
          static_cast<unsigned long>(raw_service_->lastControlError()));
      emitLine(s,info);
    } else if(!std::strncmp(compact,raw_ble::kReadPrefix,
                            std::strlen(raw_ble::kReadPrefix))) {
      uint32_t index=0,offset=0;
      const bool valid=raw_ble::parseReadArgs(
          compact+std::strlen(raw_ble::kReadPrefix),index,offset);
      if(valid && raw_service_->queueReadFromBle(
          index,offset,s.conn_id,s.generation)) {
        // Exactly one pending request, peer+generation bound. The Arduino
        // owner reads and CRC-checks flash; its eventual reply ends with '>'.
        // No speculative "success" ACK before verified data is available.
        return;
      }
      emitLine(s,"M5LOG READ REJECTED");
    } else if(!std::strncmp(compact,"ATM5LOGSTART",12)) {
      char* end=nullptr;
      const unsigned long id=std::strtoul(compact+12,&end,10);
      const bool valid=compact[12] && end!=compact+12 &&
          end && !*end && id && id<=0xFFFFFFFFUL;
      const bool accepted=valid && raw_service_->queueStartFromBle(
          static_cast<uint32_t>(id),s.conn_id,s.generation);
      emitLine(s,accepted?"M5LOG START PENDING":"M5LOG START REJECTED");
    } else if(!std::strcmp(compact,"ATM5LOGSTOP")) {
      const bool accepted=raw_service_->queueStopFromBle(
          s.conn_id,s.generation);
      emitLine(s,accepted?"M5LOG STOP PENDING":"M5LOG STOP REJECTED");
    } else emitLine(s,"M5LOG ?");
    prompt(s);
    return;
  }
#endif
#if defined(M5CAN_VEHICLE_HINT_EXPERIMENTAL)
  // Explicit, read-only, opt-in sideband. No unsolicited annotations inside
  // strict legacy M5ITEM/M5DONE frames. Does not acquire lease or send CAN.
  if(!std::strcmp(compact,"ATM5ECU")) {
    const char* state=ecu_selector_.legacy()?"LEGACY":
        ecu_selector_.fixed()?"FIXED":
        ecu_selector_.state()==EcuPhysicalSelector::State::Recheck?
          "RECHECK":"DISCOVER";
    char text[160]{};
    std::snprintf(text,sizeof(text),
        "M5CAN-ECU V=1 STATE=%s SOURCE=%08lX PROBES=%u RECHECKS=%u CONFIRMED=%u",
        state,static_cast<unsigned long>(ecu_selector_.chosenResponse()),
        unsigned(ecu_selector_.probes()),unsigned(ecu_selector_.retries()),
        ecu_selector_.fixed() && !ecu_selector_.restoredNeedsPhysicalProof()?1U:0U);
    emitLine(s,text);
    prompt(s);
    return;
  }
#endif
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
#if defined(M5CAN_ECU_SELECTION_EXPERIMENTAL)
  const bool was_ecu_group=active_ecu_group_;
  const bool was_physical=active_ecu_physical_;
  const bool was_physical_single=active_ecu_single_physical_;
  active_ecu_group_=active_ecu_physical_=active_ecu_single_physical_=false;
#endif
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


#if defined(M5CAN_ECU_SELECTION_EXPERIMENTAL)
  // The selector, unlike the legacy one-probe classifier, treats unequal
  // answers from DIFFERENT ECUs as candidate evidence, not ValuesDisagree.
  if(was_ecu_group && (cross || s.multi_pid_pair || s.ecu_probe_single)) {
    const auto previous=ecu_selector_.state();
    DiagnosticRawFrame virtual_frames[2]{};
    bool good=false;
#if defined(M5CAN_VEHICLE_HINT_EXPERIMENTAL)
    // Saved source is untrusted. Always demand a fresh functional request
    // and validate the ENTIRE RAW-backed responder set first.
    if(!was_physical && previous==EcuPhysicalSelector::State::Discover &&
       ecu_selector_.probes()==0) {
      EcuVehicleHint saved{};
      if(ecu_hint_store_.bootHint(saved)) {
        if(EcuHintCodec::matches(saved,result) &&
           ecu_selector_.restoreAfterFunctionalProof(
               result,saved.source,virtual_frames)) {
          good=true;
          Serial.printf("#ECU_HINT,fast_restore=1,source=%08lX,count=%u\\n",
                        static_cast<unsigned long>(saved.source),
                        unsigned(saved.count));
        } else {
          ecu_hint_store_.invalidateBootHint();
          Serial.println("#ECU_HINT,fast_restore=0,reason=stale_or_mismatch");
        }
      }
    }
#endif
    if(!good)good=ecu_selector_.observe(result,was_physical,virtual_frames);
#if defined(M5CAN_VEHICLE_HINT_EXPERIMENTAL)
    if(!was_physical &&
       previous==EcuPhysicalSelector::State::Discover &&
       ecu_selector_.fixed() &&
       !ecu_selector_.restoredNeedsPhysicalProof()) {
      // Never commit a candidate before it succeeds physically.
      full_probe_hint_ready_=EcuHintCodec::fromFunctional(
          result,ecu_selector_.chosenResponse(),full_probe_hint_);
    }
    if(was_physical && !good &&
       previous==EcuPhysicalSelector::State::Fixed &&
       ecu_selector_.state()==EcuPhysicalSelector::State::Discover) {
      ecu_hint_store_.invalidateBootHint();
      full_probe_hint_ready_=false;
      pid_cache_.clear();
      Serial.println("#ECU_HINT,physical_reject=1,relearning=1");
    }
    if(was_physical && good && full_probe_hint_ready_) {
      ecu_hint_store_.stage(full_probe_hint_);
      full_probe_hint_ready_=false;
    }
#endif
    const auto next=ecu_selector_.state();
    if(previous!=next) {
      pid_cache_.clear(); // no stale functional values across source states
      if(ecu_selector_.legacy())adaptive_multi_pid_.disable();
      Serial.printf("#ECU_STATE,from=%u,to=%u,source=%08lX,probes=%u,rechecks=%u,physical_ok=%lu,physical_fail=%lu\n",
                    unsigned(previous),unsigned(next),
                    static_cast<unsigned long>(ecu_selector_.chosenResponse()),
                    unsigned(ecu_selector_.probes()),unsigned(ecu_selector_.retries()),
                    static_cast<unsigned long>(ecu_selector_.physicalOk()),
                    static_cast<unsigned long>(ecu_selector_.physicalFailed()));
      for(uint8_t k=0;k<ecu_selector_.candidates();++k) {
        const auto& c=ecu_selector_.candidate(k);
        Serial.printf("#ECU_CAND,id=%08lX,success=%lu,changes=%lu,avg_us=%llu,rejected=%u\n",
                      static_cast<unsigned long>(c.can_id),
                      static_cast<unsigned long>(c.successes),
                      static_cast<unsigned long>(c.changes),
                      static_cast<unsigned long long>(c.successes?
                          c.latency_us/c.successes:0),unsigned(c.disqualified));
      }
      const auto cs=can_.stats();
      Serial.printf("#ECU_TRAFFIC,tx_attempt=%llu,tx_success=%llu,rx_total=%llu,selected_rx=%llu\n",
                    static_cast<unsigned long long>(cs.tx_attempts),
                    static_cast<unsigned long long>(cs.tx_success),
                    static_cast<unsigned long long>(cs.rx_frames),
                    static_cast<unsigned long long>(ecu_selector_.rawRxFrames()));
    }
    // Populate the existing shared 100ms cache only AFTER coherent source
    // validation and the selection state transition. Both peers use the
    // unchanged functional logical cache keys, but the same pinned ECU.
    if(good && ecu_selector_.fixed()) {
      DiagnosticRequest rpm_key{};
      rpm_key.can_id=0x18DB33F1U;rpm_key.length=2;
      rpm_key.data[0]=0x01;rpm_key.data[1]=0x0C;
      DiagnosticRequest speed_key=rpm_key;speed_key.data[1]=0x0D;
      if(!pid_cache_.rememberApprovedPair(
          rpm_key,speed_key,virtual_frames,acquired_us)) {
        // A provenance conflict is never exposed as an accepted sample.
        pid_cache_.clear();ecu_selector_.disable();
        adaptive_multi_pid_.disable();good=false;
      }
    }
    if(was_physical && !good) {
      Serial.printf("#ECU_FAILURE,source=%08lX,status=%u,response_count=%u,physical_fail=%lu\n",
                    static_cast<unsigned long>(ecu_selector_.chosenResponse()),
                    unsigned(result.status),unsigned(result.response_count),
                    static_cast<unsigned long>(ecu_selector_.physicalFailed()));
    }
    if(cross) {
      if(!primary_live || !other_live) {
        ecu_selector_.disable();
        adaptive_multi_pid_.disable();
        return; // do not deliver to a disconnected generation
      }
      const bool first_is_rpm=s.request.data[1]==0x0C;
      if(good) {
        appendFrame(s,virtual_frames[first_is_rpm?0:1]);
        appendFrame(*other,virtual_frames[first_is_rpm?1:0]);
      } else {
        emitLine(s,"NO DATA");
        emitLine(*other,"NO DATA");
      }
      s.pending=false;other->pending=false;
      if(s.batch){++s.batch_next;advanceBatch(s);}else prompt(s);
      if(other->batch){++other->batch_next;advanceBatch(*other);}
      else prompt(*other);
      return;
    }
    if(!primary_live){
      ecu_selector_.disable();
      adaptive_multi_pid_.disable();
      return;
    }
    if(s.ecu_probe_single) {
      s.ecu_probe_single=false;
      if(!primary_live){ecu_selector_.disable();return;}
      const uint8_t index=s.request.data[1]==0x0C?0:1;
      s.pending=false;
      if(good)appendFrame(s,virtual_frames[index]);
      else emitLine(s,"NO DATA");
      if(s.batch){++s.batch_next;advanceBatch(s);}else prompt(s);
      if(other_live && other->pending){
        other->pending=false;
        if(good)appendFrame(*other,virtual_frames[index]);
        else emitLine(*other,"NO DATA");
        if(other->batch){++other->batch_next;advanceBatch(*other);}
        else prompt(*other);
      }
      return;
    }
    s.multi_pid_pair=false;s.pending=false;
    emitLine(s,"M5ITEM:00:010C");
    if(good)appendFrame(s,virtual_frames[0]);else emitLine(s,"NO DATA");
    emitLine(s,"M5ITEM:00:010D");
    if(good)appendFrame(s,virtual_frames[1]);else emitLine(s,"NO DATA");
    s.batch_next=static_cast<uint8_t>(s.batch_next+2);
    advanceBatch(s);
    return;
  }
#endif

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
#if defined(M5CAN_ECU_SELECTION_EXPERIMENTAL)
  if(was_physical_single) {
    const auto old=ecu_selector_.state();
    bool valid=false;
    if(result.status==DiagnosticStatus::Ok && result.response_count==1 &&
       result.raw_frame_count==1) {
      const auto& message=result.responses[0];
      const auto& raw=result.raw_frames[0];
      valid=message.can_id==ecu_selector_.chosenResponse() &&
            message.length==(s.request.data[1]==0x0C?4U:3U) &&
            message.data[0]==0x41 && message.data[1]==s.request.data[1] &&
            raw.can_id==message.can_id &&
            raw.dlc>=message.length+1U && raw.data[0]==message.length &&
            std::memcmp(&raw.data[1],message.data,message.length)==0;
    }
    ecu_selector_.observeSingle(valid,result.status);
#if defined(M5CAN_VEHICLE_HINT_EXPERIMENTAL)
    if(valid && full_probe_hint_ready_) {
      ecu_hint_store_.stage(full_probe_hint_);
      full_probe_hint_ready_=false;
    }
    if(!valid && old==EcuPhysicalSelector::State::Fixed &&
       ecu_selector_.state()==EcuPhysicalSelector::State::Discover) {
      ecu_hint_store_.invalidateBootHint();
      full_probe_hint_ready_=false;
      pid_cache_.clear();
      Serial.println("#ECU_HINT,physical_reject=1,relearning=1");
    }
#endif
    if(old!=ecu_selector_.state()) {
      pid_cache_.clear();
      if(ecu_selector_.legacy())adaptive_multi_pid_.disable();
      Serial.printf("#ECU_SINGLE_STATE,from=%u,to=%u,source=%08lX,rechecks=%u,ok=%lu,fail=%lu\n",
                    unsigned(old),unsigned(ecu_selector_.state()),
                    static_cast<unsigned long>(ecu_selector_.chosenResponse()),
                    unsigned(ecu_selector_.retries()),
                    static_cast<unsigned long>(ecu_selector_.physicalOk()),
                    static_cast<unsigned long>(ecu_selector_.physicalFailed()));
    }
    if(!valid && result.status==DiagnosticStatus::Ok) {
      // A malformed/negative but transport-complete reply must never
      // masquerade as a positive meter sample or enter either cache.
      result.status=DiagnosticStatus::NoData;
      result.response_count=result.raw_frame_count=0;
    }
  }
#endif
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
        !ble_.isClientConnected(s.conn_id,s.generation) ||
        !ble_.responseReady(s.conn_id,s.generation)) continue;
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
        ble_.isClientConnected(peer.conn_id,peer.generation) &&
        ble_.responseReady(peer.conn_id,peer.generation);
    const DiagnosticRequest* other=
        other_pending && !s.multi_pid_pair &&
        !pid_cache_.hasFresh(peer.request,now_us) ? &peer.request : nullptr;
    const auto plan=DiagnosticBatchPlanner::plan(
        s.request,other,adaptive_multi_pid_);
    outgoing=plan.transmission;
    cross=plan.kind==DiagnosticBatchPlanner::Kind::GroupDistinct;
    const bool shared=plan.kind==DiagnosticBatchPlanner::Kind::JoinSameParameter;
#endif

#if defined(M5CAN_ECU_SELECTION_EXPERIMENTAL)
    // Keep logical BLE M5ITEM addressing unchanged. Convert ONLY requested
    // Mode01 0C/0D (or the exact paired request) to the pinned physical ID.
    if(ecu_selector_.fixed() && outgoing.can_id==0x18DB33F1U &&
       outgoing.data[0]==0x01 &&
       ((outgoing.length==2 &&
         (outgoing.data[1]==0x0C || outgoing.data[1]==0x0D)) ||
        (outgoing.length==3 && outgoing.data[1]==0x0C &&
         outgoing.data[2]==0x0D)))
      outgoing.can_id=ecu_selector_.physicalId();
#endif
#if defined(M5CAN_ECU_SELECTION_EXPERIMENTAL)
    // An RPM-only or speed-only client can drive the SAME bounded
    // functional discovery; this additional PID is authorized solely by
    // Issue54 probe policy, and never as unsolicited boot-time traffic.
    if(ecu_selector_.probing() && !demo_source_.demoActive() &&
       !s.multi_pid_pair && !cross && outgoing.can_id==0x18DB33F1U &&
       outgoing.length==2 && outgoing.data[0]==0x01 &&
       (outgoing.data[1]==0x0C || outgoing.data[1]==0x0D)) {
      s.ecu_probe_single=true;
      outgoing.length=3;outgoing.data[1]=0x0C;outgoing.data[2]=0x0D;
    }
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
#if defined(M5CAN_ECU_SELECTION_EXPERIMENTAL)
      active_ecu_group_=outgoing.length==3 && outgoing.data[0]==0x01 &&
                        outgoing.data[1]==0x0C && outgoing.data[2]==0x0D;
      active_ecu_physical_=active_ecu_group_ &&
                           outgoing.can_id!=0x18DB33F1U;
      active_ecu_single_physical_=!active_ecu_group_ &&
          outgoing.length==2 && outgoing.data[0]==0x01 &&
          (outgoing.data[1]==0x0C || outgoing.data[1]==0x0D) &&
          outgoing.can_id==ecu_selector_.physicalId() && ecu_selector_.fixed();
#endif
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

#if defined(M5CAN_LOG_STREAM_V2_EXPERIMENTAL) && defined(M5CAN_RAW_CAN_LOG_RUNTIME)
void DualBleElm::pollLogV2() {
  if(!raw_service_)return;
  // Never process external control bytes on NimBLE's GATT host task.
  BleElmTransport::LogControlMessage control{};
  for(unsigned n=0;n<4 && ble_.pollLogControl(control);++n) {
    m5can_log_stream_v2::Control request{};
    if(!m5can_log_stream_v2::decodeControl(
            control.data,control.count,request))continue;
    if(!ble_.logPeerAuthorized(control.peer,control.generation))continue;
    if(request.kind==m5can_log_stream_v2::ControlKind::Start) {
      // Explicit trigger only; never start on BLE connect / device boot.
      if(log_stream_active_ || raw_service_->state()!=
           RawCanRecordingService::State::Complete ||
         !raw_service_->ready() ||
         raw_service_->session()!=request.start.session ||
         raw_service_->committedBlocks()!=request.start.blocks)continue;
      if(!log_stream_sender_.begin(request.start.session,
                                  request.start.blocks))continue;
      log_stream_peer_=control.peer;
      log_stream_generation_=control.generation;
      log_stream_active_=true;
      log_stream_fetch_pending_=false;
      log_stream_fetch_watchdog_.clear();
      log_stream_progress_.reset(millis());
    } else if(request.kind==m5can_log_stream_v2::ControlKind::Ack) {
      if(!log_stream_active_ || control.peer!=log_stream_peer_ ||
         control.generation!=log_stream_generation_)continue;
      const uint32_t previous=log_stream_sender_.durableSequence();
      if(log_stream_sender_.acknowledge(request.ack))
        log_stream_progress_.noteAck(previous,
            log_stream_sender_.durableSequence(),millis());
    }
  }
  if(!log_stream_active_)return;
  if(!ble_.logPeerAuthorized(log_stream_peer_,log_stream_generation_) ||
     log_stream_sender_.failed()) {
    log_stream_sender_.cancel();
    log_stream_active_=log_stream_fetch_pending_=false;
    log_stream_fetch_watchdog_.clear();
    return;
  }
  // A Flash mailbox request can be rejected or never answered. Retrying it
  // must not defer a bounded 5-second deadline or destroy the source journal.
  if(log_stream_sender_.needsBlock()) {
    const uint32_t now=millis();
    log_stream_fetch_watchdog_.arm(now);
    if(log_stream_fetch_watchdog_.expired(now)) {
      log_stream_sender_.cancel();
      log_stream_active_=log_stream_fetch_pending_=false;
      log_stream_fetch_watchdog_.clear();
      return;
    }
  }
  // A single flash-owning Arduino-loop poll produces verified staging.
  // No GATT callback, BLE worker or TWAI ISR touches journal flash.
  static RawCanRecordingService::StreamBlockReply received{};
  if(raw_service_->takeStreamBlockForBle(received)) {
    // Stale mailbox replies from a prior connection generation are harmless:
    // consume them without resetting the new session's deadline or sender.
    if(m5can_log_stream_v2::BlockFetchWatchdog::matchesReply(
           log_stream_peer_,log_stream_generation_,
           log_stream_sender_.expectedBlock(),
           received.peer,received.generation,received.index)) {
      log_stream_fetch_pending_=false;
      if(received.error ||
         !log_stream_sender_.stage(
             static_cast<uint16_t>(received.index),
             received.total?received.bytes:nullptr,
             received.total,received.type==2)) {
        log_stream_sender_.cancel();
        log_stream_active_=false;
        log_stream_fetch_watchdog_.clear();
        return;
      }
      log_stream_fetch_watchdog_.clear();
      log_stream_progress_.reset(millis());
    }
  }
  if(log_stream_sender_.completed()) {
    log_stream_fetch_watchdog_.clear();
    log_stream_active_=false;return;
  }
  if(log_stream_sender_.needsBlock() && !log_stream_fetch_pending_) {
    log_stream_fetch_pending_=raw_service_->queueStreamBlockFromBle(
        log_stream_sender_.expectedBlock(),
        log_stream_peer_,log_stream_generation_);
  }
  // CAN PID dispatch and ASCII responses ran first. Burst at most three
  // V2 frames per application poll. EACH notifyLogFrame rechecks both
  // clients' pending ELM replies, MTU, bonded identity, generation and
  // NimBLE mbuf/controller credit before consuming the next V2 frame.
  // A busy host returns immediately; there is no spin or Flash access here.
  uint8_t frame[m5can_log_stream_v2::kMaxNotification]{};
  for(unsigned burst=0;
      burst<m5can_log_stream_v2::PeerQosPolicy::kMaxLogFramesPerPoll;
      ++burst) {
    const size_t n=log_stream_sender_.peek(frame,sizeof(frame));
    if(!n || !ble_.notifyLogFrame(log_stream_peer_,log_stream_generation_,
                                   frame,n))break;
    if(!log_stream_sender_.queued()) {
      log_stream_sender_.cancel();log_stream_active_=false;return;
    }
    // Keep ACK deadline anchored at the last accepted notification.
    log_stream_progress_.noteNotification(millis());
  }
  // Withhold credit indefinitely only until a bounded ACK deadline.
  // Retransmission reconstructs the original immutable staged block.
  // END requires a whole-file SD CRC reread on the receiver's logger
  // task. Do not misdiagnose that legitimate long readback as lost BLE.
  const uint32_t ack_deadline_ms=
      log_stream_sender_.awaitingFinalAck()?
          m5can_log_stream_v2::ProgressWatchdog::kFinalAckTimeoutMs:
          m5can_log_stream_v2::ProgressWatchdog::kDataAckTimeoutMs;
  if(log_stream_sender_.outstanding() &&
     log_stream_progress_.elapsed(millis(),ack_deadline_ms)) {
    if(!log_stream_sender_.retryUnacked()) {
      log_stream_sender_.cancel();log_stream_active_=false;
    } else log_stream_progress_.reset(millis());
  }
}
#endif

void DualBleElm::poll() {
  BleClientEvent event_item{};
  for(int i=0;i<512 && ble_.pollClientEvent(event_item);++i)
    event(event_item);
  onResult();
#if defined(M5CAN_RAW_CAN_LOG_RUNTIME)
  if(raw_service_) {
    // BLE app task has a strict 128-byte stack frame budget. Static storage
    // is safe here: only the single BLE ELM worker ever executes poll().
    static RawCanRecordingService::ExportReply reply{};
    if(raw_service_->takeReadForBle(reply)) {
      Session* recipient=find(reply.peer,reply.generation);
      // Never route a response to a reused BLE connection id after reconnect.
      if(recipient) {
        static char line[250]{};
        line[0]='\0';
        if(reply.error) {
          std::snprintf(line,sizeof(line),
              "M5LOG READ ERROR BLOCK=%lu OFFSET=%lu CODE=%u",
              static_cast<unsigned long>(reply.index),
              static_cast<unsigned long>(reply.offset),
              static_cast<unsigned>(reply.error));
        } else {
          static char hex[raw_ble::kChunkBytes*2+1]{};
          if(!raw_ble::hexEncode(reply.bytes,reply.count,
                                 hex,sizeof(hex))) {
            emitLine(*recipient,"M5LOG READ INTERNAL");
            prompt(*recipient);
          } else {
            std::snprintf(line,sizeof(line),
                "M5LOG DATA BLOCK=%lu OFFSET=%lu SIZE=%u TOTAL=%u TYPE=%u CRC=%08lX HEX=%s",
                static_cast<unsigned long>(reply.index),
                static_cast<unsigned long>(reply.offset),
                static_cast<unsigned>(reply.count),
                static_cast<unsigned>(reply.total),
                static_cast<unsigned>(reply.type),
                static_cast<unsigned long>(reply.crc),hex);
          }
        }
        if(line[0]) {
          emitLine(*recipient,line);
          prompt(*recipient);
        }
      }
    }
  }
#endif
  for(auto& s:sessions_)
    if(s.active && s.batch && !s.pending)advanceBatch(s);
  dispatch();
  ble_.pollTx();
#if defined(M5CAN_LOG_STREAM_V2_EXPERIMENTAL) && defined(M5CAN_RAW_CAN_LOG_RUNTIME)
  pollLogV2();
#endif
}
} // namespace m5can

#endif // M5CAN_DUAL_BLE_EXPERIMENTAL
