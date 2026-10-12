#include "dual_ble_elm.h"
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "app_config.h"
#include "diagnostic_policy.h"
#include "elm_response_formatter.h"
#include "serial_protocol.h"

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
    ble_.writeTo(s.conn_id,s.generation,s.output,s.output_size);
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
      const uint64_t now_us=static_cast<uint64_t>(millis())*1000ULL;
      if (!pid_cache_.hasFresh(first,now_us) &&
          !pid_cache_.hasFresh(second,now_us) &&
          adaptive_multi_pid_.eligible(first,second)) {
        const DiagnosticRequest grouped=
            AdaptiveMultiPid::group(first,second);
        if (DiagnosticPolicy::allowedReadRequest(grouped)) {
          s.request=grouped;
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
    // Exactly one outstanding command per BLE client; the other client's
    // independent ELM session remains free to enqueue a request.
    static const uint8_t busy[]={'B','U','S','Y','\r','>'};
    ble_.writeTo(s.conn_id,s.generation,busy,sizeof(busy));
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
        if (!std::strcmp(compact,"ATM5CAP")) {
          char caps[200]{};
          std::snprintf(caps,sizeof(caps),"%s DEMO=1 MODE=%s",
                        parsed.reply,demo_source_.demoActive()?"DEMO":"CAN");
          emitLine(s,caps);
        } else emitLine(s,parsed.reply);
      }
      break;
    case ElmAction::BatchRead:
      if (can_.faultLocked()) emitLine(s,"M5CAN TX LOCKED");
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
      if (!DiagnosticPolicy::allowedReadRequest(s.request)) emitLine(s,"?");
      else if (demo_source_.demoActive()) {
        DiagnosticRawFrame frames[2]{};
        const uint8_t n=parsed.request.length==2 && parsed.request.data[0]==0x01
            ? DemoDrive::frames(parsed.request.data[1],millis(),frames):0;
        if (!n) emitLine(s,"NO DATA");
        for(uint8_t i=0;i<n;++i) appendFrame(s,frames[i]);
      } else if (can_.faultLocked()) emitLine(s,"M5CAN TX LOCKED");
      else {
        s.pending=true;
        prompt_now=false;
      }
      break;
    case ElmAction::LeaseAcquire:
      emitLine(s,can_.acquireLease(cfg::kTxLeaseDefaultMs)?"OK":"CAN ERROR");
      break;
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
}

void DualBleElm::onResult() {
  if (active_query_owner_<0) return;
  DiagnosticResult result{};
  if (!can_.pollQueryResult(result)) return;
  const int index=active_query_owner_;
  const int second=active_secondary_owner_;
  const bool cross=active_cross_group_;
  active_query_owner_=-1;
  active_secondary_owner_=-1;
  active_cross_group_=false;
  Session& s=sessions_[index];
  const bool primary_live=s.active &&
      s.generation==active_query_generation_;
  Session* other=second<0?nullptr:&sessions_[second];
  const bool other_live=other && other->active &&
      ble_.isClientConnected(other->conn_id,other->generation);
  const uint64_t now_us=static_cast<uint64_t>(millis())*1000ULL;

  if (cross) {
    // The pair was constructed solely from TWO ALREADY PENDING peers.
    // Losing one peer invalidates the speculative support decision. The
    // other peer retains its individual pending request for normal retry.
    if (!primary_live || !other_live) {
      adaptive_multi_pid_.disable();
      return;
    }
    DiagnosticRawFrame virtual_frames[2]{};
    uint32_t responder=0;
    const bool first_success=
        adaptive_multi_pid_.support()==AdaptiveMultiPid::Support::Unknown;
    const auto decision=
        adaptive_multi_pid_.evaluate(result,virtual_frames,responder);
    if (decision==AdaptiveMultiPid::Decision::UseGrouped) {
      const bool s_rpm=s.request.data[1]==0x0C;
      const DiagnosticRequest& rpm=s_rpm?s.request:other->request;
      const DiagnosticRequest& speed=s_rpm?other->request:s.request;
      pid_cache_.rememberApprovedPair(rpm,speed,virtual_frames,now_us);
      // The per-item M5ITEM lines were already emitted when staged.
      // Never rewrite or cross-deliver a peer's batch response.
      appendFrame(s,virtual_frames[s_rpm?0:1]);
      appendFrame(*other,virtual_frames[s_rpm?1:0]);
      s.pending=false;
      other->pending=false;
      if (first_success)
        Serial.printf("#MULTIPID,enabled,ecu=%08lX\\n",
                      static_cast<unsigned long>(responder));
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
                            s_rpm?other->request:s.request,now_us);
    }
    // RetryLegacy leaves both individual requests pending. The next
    // dispatch will observe the disabled capability and send them one by
    // one, respecting the existing CAN owner's per-PID 50ms guard.
    return;
  }

  if (!primary_live) {
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
    const auto decision=
        adaptive_multi_pid_.evaluate(result,virtual_frames,responder);
    if (decision==AdaptiveMultiPid::Decision::UseGrouped) {
      const DiagnosticRequest rpm=requestOf(s.items[s.batch_next],s.elm);
      const DiagnosticRequest speed=requestOf(s.items[s.batch_next+1],s.elm);
      pid_cache_.rememberApprovedPair(rpm,speed,virtual_frames,now_us);
      emitLine(s,"M5ITEM:00:010C");
      appendFrame(s,virtual_frames[0]);
      emitLine(s,"M5ITEM:00:010D");
      appendFrame(s,virtual_frames[1]);
      s.batch_next=static_cast<uint8_t>(s.batch_next+2);
      if (first_success)
        Serial.printf("#MULTIPID,enabled,ecu=%08lX\\n",
                      static_cast<unsigned long>(responder));
    } else {
      Serial.printf("#MULTIPID,disabled,status=%u,ecu=%08lX\\n",
                    static_cast<unsigned>(result.status),
                    static_cast<unsigned long>(responder));
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
      cacheSafePartialGroup(result,rpm,speed,now_us);
    }
    advanceBatch(s);
    return;
  }

  // A successful single PID updates the same cache used by both peers,
  // whether it was sent alone or followed a failed group probe. Cache
  // failures invalidate ONLY the requested PID, never unrelated samples.
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
  pid_cache_.rememberSingle(s.request,result,now_us);
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
    if (s.multi_pid_pair &&
        adaptive_multi_pid_.support()==AdaptiveMultiPid::Support::Disabled) {
      s.multi_pid_pair=false;
      s.pending=false;
      advanceBatch(s);
      if (!s.pending) continue;
    }
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
    const uint64_t now_us=static_cast<uint64_t>(millis())*1000ULL;
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
    if (!s.multi_pid_pair &&
        pid_cache_.lookup(s.request,now_us,cache_workspace_)) {
      finishSingle(s,cache_workspace_);
      // Another device may now be able to use that same PID value.
      continue;
    }
#endif

    // At most two BLE peers: do not need a heap-backed global queue.
    // Both waiting peer intents may share ONE physical read, or an
    // eligible RPM+speed pair can be bundled across DIFFERENT peers.
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
    const int j=1-i;
#endif
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
    Session& peer=sessions_[j];
#endif
    bool cross=false;
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
    bool shared=false;
#endif
    DiagnosticRequest outgoing=s.request;
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
    const bool other_pending=peer.active && peer.pending &&
        !peer.multi_pid_pair &&
        ble_.isClientConnected(peer.conn_id,peer.generation);
    const bool can_pair=other_pending && !s.multi_pid_pair &&
        !pid_cache_.hasFresh(peer.request,now_us);
    if (can_pair) {
      const DiagnosticRequest* rpm=nullptr;
      const DiagnosticRequest* speed=nullptr;
      if (s.request.length==2 && s.request.data[0]==0x01 &&
          peer.request.length==2 && peer.request.data[0]==0x01 &&
          s.request.data[1]==0x0C && peer.request.data[1]==0x0D) {
        rpm=&s.request;speed=&peer.request;
      } else if (peer.request.length==2 && peer.request.data[0]==0x01 &&
                 s.request.length==2 && s.request.data[0]==0x01 &&
                 peer.request.data[1]==0x0C && s.request.data[1]==0x0D) {
        rpm=&peer.request;speed=&s.request;
      }
      if (rpm && speed && adaptive_multi_pid_.eligible(*rpm,*speed)) {
        outgoing=AdaptiveMultiPid::group(*rpm,*speed);
        cross=DiagnosticPolicy::allowedReadRequest(outgoing);
      }
    }
#endif
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
    if (!cross && other_pending && !s.multi_pid_pair &&
        s.request.can_id==peer.request.can_id &&
        s.request.length==peer.request.length &&
        s.request.timeout_ms==peer.request.timeout_ms &&
        std::memcmp(s.request.data,peer.request.data,s.request.length)==0)
      shared=true;
#endif

    if (!can_.acquireLease(cfg::kTxLeaseMaxMs)) {
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
      active_query_owner_=i;
      active_query_generation_=s.generation;
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
      active_secondary_owner_=(shared || cross)?j:-1;
#else
      active_secondary_owner_=-1;
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
}
} // namespace m5can

#endif // M5CAN_DUAL_BLE_EXPERIMENTAL
