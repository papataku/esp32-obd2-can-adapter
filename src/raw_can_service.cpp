#include "raw_can_service.h"
#include <Arduino.h>

namespace m5can {

bool RawCanRecordingService::begin() {
  const bool ready=initializeRawCanJournal();
  const auto& status=rawCanJournal().status();
  session_id_.store(ready?status.session:0,std::memory_order_release);
  retained_dropped_.store(ready?status.last_dropped:0,
                          std::memory_order_release);
  const State initial=!ready ? State::Unavailable :
      status.corrupt || (status.blocks && !status.completed)
          ? State::RecoveryRequired :
      status.blocks==0 ? State::Idle : State::Complete;
  state_.store(initial,std::memory_order_release);
  return ready;
}
// NimBLE's first connection may have handle 0. Generation 0 is the
// invalid/unassigned sentinel; reject it, not an otherwise valid handle 0.
// Matching owner handle AND nonzero generation still fences stale peers.
bool RawCanRecordingService::queueStartFromBle(
    uint32_t session,uint16_t peer,uint32_t generation) {
  if(!session || !generation || state()!=State::Idle ||
     requested_session_.load(std::memory_order_acquire))return false;
  // DualBleElm is the only BLE producer; release publishes both peer fields.
  requested_peer_.store(peer,std::memory_order_relaxed);
  requested_generation_.store(generation,std::memory_order_relaxed);
  requested_session_.store(session,std::memory_order_release);
  return true;
}
bool RawCanRecordingService::queueStopFromBle(
    uint16_t peer,uint32_t generation) {
  if(!generation || state()!=State::Recording ||
     owner_peer_.load(std::memory_order_acquire)!=peer ||
     owner_generation_.load(std::memory_order_acquire)!=generation)
    return false;
  bool expected=false;
  return requested_stop_.compare_exchange_strong(
      expected,true,std::memory_order_acq_rel);
}
bool RawCanRecordingService::queueReadFromBle(
    uint32_t index,uint32_t offset,uint16_t peer,uint32_t generation) {
  const auto s=state();
  if(!generation ||
     (s!=State::Complete && s!=State::RecoveryRequired) ||
     !rawCanJournal().status().ready)return false;
  uint8_t expected=ReadIdle;
  if(!read_state_.compare_exchange_strong(
      expected,ReadFilling,std::memory_order_acq_rel))return false;
  read_reply_=ExportReply{};
  read_reply_.peer=peer;
  read_reply_.generation=generation;
  read_reply_.index=index;
  read_reply_.offset=offset;
  read_state_.store(ReadQueued,std::memory_order_release);
  return true;
}

bool RawCanRecordingService::takeReadForBle(ExportReply& reply) {
  if(read_state_.load(std::memory_order_acquire)!=ReadReady)return false;
  reply=read_reply_;
  read_state_.store(ReadIdle,std::memory_order_release);
  return true;
}

// Called ONLY by Arduino loop, never by BLE poll/notify callback.
void RawCanRecordingService::pollRead() {
  if(read_state_.load(std::memory_order_acquire)!=ReadQueued)return;
  // Reuse the CAN-idle reservation also used by evidence and RAW writes.
  // CAN queries take priority. The queued request is retried on next poll.
  if(!can_.tryBeginLogCheckpoint())return;
  read_state_.store(ReadProcessing,std::memory_order_release);
  auto& reply=read_reply_;
  const auto state_now=state();
  const auto& status=rawCanJournal().status();
  if((state_now!=State::Complete && state_now!=State::RecoveryRequired) ||
     !status.ready) {
    reply.error=1; // export unavailable
  } else if(reply.index>=status.blocks) {
    reply.error=2; // unknown committed block
  } else {
    uint16_t total=0;
    RawCanJournal::RecordType type=RawCanJournal::End;
    if(!rawCanJournal().readBlock(reply.index,read_buffer_,
          sizeof(read_buffer_),total,type)) {
      reply.error=3; // damaged or unreadable block; no unverified bytes
    } else if(reply.offset>total) {
      reply.error=4; // beyond end of block
    } else {
      reply.total=total;
      reply.type=static_cast<uint8_t>(type);
      const uint32_t left=static_cast<uint32_t>(total)-reply.offset;
      reply.count=static_cast<uint8_t>(
          left>raw_ble::kChunkBytes?raw_ble::kChunkBytes:left);
      if(reply.count)
        std::memcpy(reply.bytes,read_buffer_+reply.offset,reply.count);
      reply.crc=raw_ble::crc32(reply.bytes,reply.count);
    }
  }
  can_.endLogCheckpoint();
  read_state_.store(ReadReady,std::memory_order_release);
}

#if defined(M5CAN_LOG_STREAM_V2_EXPERIMENTAL)
bool RawCanRecordingService::queueStreamBlockFromBle(
    uint32_t index,uint16_t peer,uint32_t generation) {
  const auto& status=rawCanJournal().status();
  if(!generation || state()!=State::Complete ||
     !status.ready || !status.completed || index>=status.blocks)
    return false;
  uint8_t expected=ReadIdle;
  if(!stream_state_.compare_exchange_strong(
         expected,ReadFilling,std::memory_order_acq_rel))return false;
  stream_reply_=StreamBlockReply{};
  stream_reply_.peer=peer;
  stream_reply_.generation=generation;
  stream_reply_.index=index;
  stream_state_.store(ReadQueued,std::memory_order_release);
  return true;
}
bool RawCanRecordingService::takeStreamBlockForBle(StreamBlockReply& reply) {
  if(stream_state_.load(std::memory_order_acquire)!=ReadReady)return false;
  reply=stream_reply_;
  stream_state_.store(ReadIdle,std::memory_order_release);
  return true;
}
void RawCanRecordingService::pollStreamRead() {
  if(stream_state_.load(std::memory_order_acquire)!=ReadQueued)return;
  // Actual TWAI owner transaction has precedence over flash storage reads.
  if(!can_.tryBeginLogCheckpoint())return;
  stream_state_.store(ReadProcessing,std::memory_order_release);
  auto& reply=stream_reply_;
  const auto& status=rawCanJournal().status();
  if(state()!=State::Complete || !status.ready ||
     !status.completed || reply.index>=status.blocks) {
    reply.error=1;
  } else {
    uint16_t size=0;
    RawCanJournal::RecordType type=RawCanJournal::End;
    if(!rawCanJournal().readBlock(reply.index,reply.bytes,
                                  sizeof(reply.bytes),size,type)) {
      reply.error=2;  // no payload supplied on invalid journal CRC
    } else if((reply.index+1==status.blocks)!=(type==RawCanJournal::End)) {
      reply.error=3;  // archive END must be last and unique
    } else {
      reply.total=size;
      reply.type=static_cast<uint8_t>(type);
    }
  }
  if(reply.error) {
    reply.total=0;reply.type=0;  // fail closed
  }
  can_.endLogCheckpoint();
  stream_state_.store(ReadReady,std::memory_order_release);
}
#endif

bool RawCanRecordingService::start(uint32_t session_id) {
  // No auto-overwrite, no "record on boot", and no start before a verified
  // partition migration. A previous session must be exported and explicit
  // reclaim implemented/verified before accepting another capture.
  const auto& journal=rawCanJournal().status();
  if(!session_id || !journal.ready || journal.blocks ||
     recording_ || stopping_ || fault_ || has_pending_frame_)return false;
  encoder_.reset();
  if(!can_.rawCapture().start(session_id))return false;
  retained_dropped_.store(0,std::memory_order_release);
  session_id_.store(session_id,std::memory_order_release);
  last_flush_ms_=millis();recording_=true;
  state_.store(State::Recording,std::memory_order_release);
  return true;
}
bool RawCanRecordingService::stop() {
  if(!recording_ || stopping_)return false;
  can_.rawCapture().stop();
  recording_=false;stopping_=true;
  state_.store(State::Stopping,std::memory_order_release);
  return true;
}
void RawCanRecordingService::fail() {
  can_.rawCapture().stop();
  recording_=false;stopping_=false;fault_=true;
  state_.store(State::Fault,std::memory_order_release);
}

bool RawCanRecordingService::commit() {
  if(!encoder_.count())return true;
  // Busy CAN requests delay flash writes, not CAN transactions. Buffering
  // pressure is visible in RawCanCapture::lost; never silently skip frames.
  if(!can_.tryBeginLogCheckpoint())return false;
  const bool encoded=encoder_.finalize();
  const bool ok=encoded && rawCanJournal().append(session_id_,
      RawCanJournal::Data,encoder_.data(),
      static_cast<uint16_t>(encoder_.size()),can_.rawCapture().lost());
  can_.endLogCheckpoint();
  if(!ok){fail();return false;}
  encoder_.reset();
  last_flush_ms_=millis();
  return true;
}
void RawCanRecordingService::poll() {
  const uint32_t queued=requested_session_.exchange(
      0,std::memory_order_acq_rel);
  if(queued) {
    owner_peer_.store(requested_peer_.load(std::memory_order_relaxed),
                      std::memory_order_release);
    owner_generation_.store(
        requested_generation_.load(std::memory_order_relaxed),
        std::memory_order_release);
    if(!start(queued)) {
      owner_peer_.store(0,std::memory_order_release);
      owner_generation_.store(0,std::memory_order_release);
      last_control_error_.store(1,std::memory_order_release);
    } else {
      last_control_error_.store(0,std::memory_order_release);
    }
  }
  if(requested_stop_.exchange(false,std::memory_order_acq_rel)) {
    if(!stop())last_control_error_.store(2,std::memory_order_release);
  }
  // Never read flash from the BLE worker; reply to one queued chunk per tick.
  pollRead();
#if defined(M5CAN_LOG_STREAM_V2_EXPERIMENTAL)
  pollStreamRead();
#endif
  if((!recording_ && !stopping_)||fault_)return;
  // Never spend unbounded time draining the queue on the display/USB owner.
  for(unsigned i=0;i<48;++i) {
    RawCanFrame f{};
    if(has_pending_frame_) {
      f=pending_frame_;
      has_pending_frame_=false;
    } else if(!can_.rawCapture().pop(f))break;
    if(encoder_.append(f))continue;
    if(!encoder_.count()){fail();return;}
    pending_frame_=f;has_pending_frame_=true;
    if(!commit())return;
    // The pending frame must not be re-encoded until the next poll tick.
    break;
  }
  if(!has_pending_frame_ && encoder_.count() &&
     (stopping_ || static_cast<uint32_t>(millis()-last_flush_ms_)>=1000)) {
    if(!commit())return;
  }
  if(stopping_ && !has_pending_frame_ && !encoder_.count() &&
     can_.rawCapture().quiescent() && !can_.rawCapture().queued()) {
    if(!can_.tryBeginLogCheckpoint())return;
    const bool success=rawCanJournal().append(session_id_,
        RawCanJournal::End,nullptr,0,can_.rawCapture().lost());
    can_.endLogCheckpoint();
    if(!success){fail();return;}
    stopping_=false;
    state_.store(State::Complete,std::memory_order_release);
  }
}
} // namespace m5can
