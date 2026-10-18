#pragma once
// Bounded V2 RAW journal packetizer. It never reads flash or touches BLE:
// Arduino/CAN-idle owner stages ONE independently validated immutable block;
// the BLE app may then enqueue up to 16 notifications without per-frame RTT.
// Only an authenticated receiver's SD-durable cursor may advance credit.
// This class is bench-only until the paired, isolated GATT transport exists.
#include "m5can_log_stream_v2.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace m5can_log_stream_v2 {
class BlockSender {
 public:
  static constexpr uint8_t kMaxRetries=4;
  bool begin(uint32_t session,uint16_t committed_blocks) {
    if(!session || !committed_blocks || committed_blocks>4096 ||
       !gate_.reset(session))return false;
    session_=session;blocks_=committed_blocks;
    block_=length_=0;loaded_=ended_=failed_=false;
    retries_=0;last_seq_=0;complete_=false;return true;
  }
  // Staging MUST run on the flash owner at a verified CAN-idle checkpoint.
  // block_data is already verified against the journal's committed CRC;
  // stream memory is independent of the caller's flash buffer afterwards.
  bool stage(uint16_t index,const uint8_t* block_data,uint16_t length,
             bool is_end=false) {
    if(!session_ || loaded_ || ended_ || failed_ ||
       gate_.outstanding() || index!=gate_.nextBlock() ||
       index>=blocks_ || length>kMaxBlockBytes ||
       (is_end ? (length || block_data || index!=blocks_-1):
                 (!length || !block_data || index==blocks_-1)))return false;
    if(length)std::memcpy(data_,block_data,length);
    block_=index;length_=length;ended_=is_end;loaded_=true;
    return true;
  }
  // Peek is side effect free. Caller invokes queued() only after the exact
  // notification was accepted by a bounded per-peer BLE output queue.
  size_t peek(uint8_t* out,size_t cap)const {
    if(!out || !loaded_ || failed_ || !gate_.maySend())return 0;
    Frame frame{};
    if(!build(frame))return 0;
    return encode(frame,out,cap);
  }
  bool queued() {
    if(!loaded_ || failed_ || !gate_.maySend())return false;
    Frame frame{};
    return build(frame) && gate_.sent(frame);
  }
  bool acknowledge(const Frame& ack) {
    if(!loaded_ || failed_ || ack.kind!=Kind::Ack ||
       !gate_.acknowledge(ack))return false;
    if(gate_.durableNext()!=last_seq_) {
      last_seq_=gate_.durableNext();retries_=0;
    }
    if(ended_ && gate_.completeAcked()) {
      loaded_=false;ended_=false;complete_=true;
    } else if(!ended_ && gate_.durableBlock()>block_) {
      loaded_=false;
    }
    return true;
  }
  // The caller detects notification backpressure or a bounded ACK deadline.
  // No FLASH access, CAN request, unlimited busy wait or silent data skip.
  bool retryUnacked() {
    if(!loaded_ || failed_ || !gate_.outstanding())return false;
    if(retries_>=kMaxRetries){failed_=true;return false;}
    ++retries_;gate_.rewindUnacked();return true;
  }
  bool needsBlock()const {
    return session_ && !loaded_ && !failed_ && !complete_ &&
           gate_.outstanding()==0;
  }
  bool awaitingFinalAck()const{return loaded_ && ended_ && !complete_;}
  bool completed()const{return complete_;}
  bool failed()const{return failed_;}
  uint32_t durableSequence()const{return gate_.durableNext();}
  uint16_t expectedBlock()const{return gate_.nextBlock();}
  uint16_t expectedOffset()const{return gate_.nextOffset();}
  uint32_t outstanding()const{return gate_.outstanding();}
  uint8_t retries()const{return retries_;}
  void cancel(){session_=0;loaded_=false;failed_=true;}
 private:
  bool build(Frame& frame)const {
    if(gate_.nextBlock()!=block_ || (ended_ && gate_.nextOffset()!=0))
      return false;
    frame.session=session_;frame.sequence=gate_.nextToSend();
    frame.block=block_;frame.offset=gate_.nextOffset();
    if(ended_) {
      frame.kind=Kind::End;return true;
    }
    if(frame.offset>=length_)return false; // wait for durable ACK
    frame.kind=Kind::Data;frame.total=length_;
    const uint16_t remain=static_cast<uint16_t>(length_-frame.offset);
    frame.length=remain>kMaxPayload?kMaxPayload:remain;
    frame.data=data_+frame.offset;return true;
  }
  WindowGate gate_{};
  uint8_t data_[kMaxBlockBytes]{};
  uint32_t session_=0,last_seq_=0;
  uint16_t blocks_=0,block_=0,length_=0;
  uint8_t retries_=0;
  bool loaded_=false,ended_=false,failed_=false,complete_=false;
};
}
