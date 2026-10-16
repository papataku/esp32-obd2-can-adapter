#include "raw_can_service.h"
#include <Arduino.h>

namespace m5can {

bool RawCanRecordingService::begin() {
  return initializeRawCanJournal();
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
  session_id_=session_id;last_flush_ms_=millis();recording_=true;
  return true;
}
bool RawCanRecordingService::stop() {
  if(!recording_ || stopping_)return false;
  can_.rawCapture().stop();
  recording_=false;stopping_=true;
  return true;
}
void RawCanRecordingService::fail() {
  can_.rawCapture().stop();
  recording_=false;stopping_=false;fault_=true;
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
     !can_.rawCapture().queued()) {
    if(!can_.tryBeginLogCheckpoint())return;
    const bool success=rawCanJournal().append(session_id_,
        RawCanJournal::End,nullptr,0,can_.rawCapture().lost());
    can_.endLogCheckpoint();
    if(!success){fail();return;}
    stopping_=false;
  }
}
} // namespace m5can
