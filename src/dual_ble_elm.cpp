#include "dual_ble_elm.h"
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "app_config.h"
#include "diagnostic_policy.h"
#include "elm_response_formatter.h"
#include "serial_protocol.h"
#include "drive_log.h"

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
    if (s->multi_pid_pair) adaptive_multi_pid_.disable();
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
      if (adaptive_multi_pid_.eligible(first,second)) {
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

void DualBleElm::onResult() {
  if (active_query_owner_<0) return;
  DiagnosticResult result{};
  if (!can_.pollQueryResult(result)) return;
  const int index=active_query_owner_;
  active_query_owner_=-1;
  Session& s=sessions_[index];
  if (!s.active || s.generation!=active_query_generation_) return;
  if (s.multi_pid_pair) {
    s.multi_pid_pair=false;
    s.pending=false;
    DiagnosticRawFrame virtual_frames[2]{};
    uint32_t responder=0;
    const bool first_success=
        adaptive_multi_pid_.support()==AdaptiveMultiPid::Support::Unknown;
    const auto decision=
        adaptive_multi_pid_.evaluate(result,virtual_frames,responder);
    driveLog().outcome(decision==AdaptiveMultiPid::Decision::UseGrouped
        ? DriveLog::Kind::GroupEnabled
        : decision==AdaptiveMultiPid::Decision::RetryLegacy
          ? DriveLog::Kind::GroupFallback : DriveLog::Kind::GroupStopped,
        s.conn_id,result,responder);
    if (decision==AdaptiveMultiPid::Decision::UseGrouped) {
      // Preserve the existing ATM5B item-by-item wire contract.
      // These are decoded virtual ELM frames, NOT physical RAW captures.
      emitLine(s,"M5ITEM:00:010C");
      appendFrame(s,virtual_frames[0]);
      emitLine(s,"M5ITEM:00:010D");
      appendFrame(s,virtual_frames[1]);
      s.batch_next=static_cast<uint8_t>(s.batch_next+2);
      if (first_success)
        Serial.printf("#MULTIPID,enabled,ecu=%08lX\n",
                      static_cast<unsigned long>(responder));
    } else {
      adaptive_multi_pid_.disable();
      Serial.printf("#MULTIPID,disabled,status=%u,ecu=%08lX\n",
                    static_cast<unsigned>(result.status),
                    static_cast<unsigned long>(responder));
      // Only a valid-but-incomplete reply, negative response, protocol
      // failure or ordinary timeout may trigger per-item CAN retries.
      // On bus/lease/driver failures never initiate more CAN transmissions.
      if (decision==AdaptiveMultiPid::Decision::Stop) {
        emitLine(s,"M5ITEM:00:010C");
        appendResult(s,result);
        emitLine(s,"M5ITEM:00:010D");
        appendResult(s,result);
        s.batch_next=static_cast<uint8_t>(s.batch_next+2);
        // The preceding CAN/lease/policy failure may have changed our
        // transmit authorization. Do not proceed to the other batch items.
        s.batch=false;
        emitLine(s,"M5DONE");
        prompt(s);
        return;
      }
    }
    advanceBatch(s);
    return;
  }
  appendResult(s,result);
  s.pending=false;
  if (s.batch) {
    ++s.batch_next;
    advanceBatch(s);
  } else prompt(s);
}

void DualBleElm::dispatch() {
  if (active_query_owner_>=0 || can_.queryPending()) return;
  for (int scan=0;scan<2;++scan) {
    const int i=(round_robin_+scan)%2;
    Session& s=sessions_[i];
    if (!s.active || !s.pending ||
        !ble_.isClientConnected(s.conn_id,s.generation)) continue;
    // A second BLE client may already have staged the same probe when the
    // first client's ECU response disables grouping. Re-evaluate right at
    // dispatch; never send a second unsupported grouped CAN request.
    if (s.multi_pid_pair &&
        adaptive_multi_pid_.support()==AdaptiveMultiPid::Support::Disabled) {
      s.multi_pid_pair=false;
      s.pending=false;
      advanceBatch(s); // unchanged batch index => legacy individual reads
      if (!s.pending) continue;
    }
    if (!can_.acquireLease(cfg::kTxLeaseMaxMs)) {
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
        // No TX permission: stop the entire remaining batch, not just
        // the grouped pair, and never automatically re-probe.
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
    if (can_.submitQuery(s.request)) {
      driveLog().submitted(s.request);
      active_query_owner_=i;
      active_query_generation_=s.generation;
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
