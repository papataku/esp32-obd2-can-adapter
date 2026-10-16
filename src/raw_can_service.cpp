#include "raw_can_service.h"
#include <Arduino.h>

namespace m5can {

bool RawCanRecordingService::begin() {
  const bool ready=initializeRawCanJournal();
  const auto& status=rawCanJournal().status();
  const State initial=!ready ? State::Unavailable :
      status.blocks==0 ? State::Idle :
      status.completed ? State::Complete : State::RecoveryRequired;
  state_.store(initial,std::memory_order_release);
  return ready;
}
bool RawCanRecordingService::queueStartFromBle(
    uint32_t session,uint16_t peer,uint32_t generation) {
  if(!session || !peer || !generation || state()!=State::Idle ||
     requested_session_.load(std::memory_order_acquire))return false;
  // DualBleElm is the only BLE producer; release publishes both peer fields.
  requested_peer_.store(peer,std::memory_order_relaxed);
  requested_generation_.store(generation,std::memory_order_relaxed);
  requested_session_.store(session,std::memory_order_release);
  return true;
}
bool RawCanRecordingService::queueStopFromBle(
    uint16_t peer,uint32_t generation) {
  if(!peer || !generation || state()!=State::Recording ||
     owner_peer_.load(std::memory_order_acquire)!=peer ||
     owner_generation_.load(std::memory_order_acquire)!=generation)
    return false;
  bool expected=false;
  return requested_stop_.compare_exchange_strong(
      expected,true,std::memory_order_acq_rel);
}
bool RawCanRecordingService::start(uint32_t session_id) {
  // No auto-overwrite, no "record on boot", and no start before a verified
  // partition migration. A previous session must be exported and explicit
  // reclaim implemented/verified before accepting another capture.
  const auto& journal=rawCanJournal().status();
  if(!session_id || !journal.ready || journal.blocks ||
     recording_ || stopping_ || fault_ || has_pending_frame_)return false;
  encoder_.reset();
  if(!can_.rawCapture().start(session_id))return false;
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
