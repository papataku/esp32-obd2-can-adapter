#pragma once
// Opt-in M5CAN log streaming V2 wire contract. Identical on meter/adapter.
// No GATT, filesystem or vehicle-CAN side effects.
#include <cstddef>
#include <cstdint>
namespace m5can_log_stream_v2 {
constexpr uint8_t kVersion=2;
constexpr size_t kAttMtu=247;
constexpr size_t kMaxNotification=kAttMtu-3;
constexpr size_t kHeader=24;
constexpr size_t kTrailer=4;
constexpr size_t kMaxPayload=kMaxNotification-kHeader-kTrailer;
constexpr uint32_t kWindowFrames=16;
constexpr uint16_t kMaxBlockBytes=3616;
static_assert(kMaxPayload==216,"MTU 247 notification payload budget");
enum class Kind:uint8_t {Data=1,End=2,Ack=3};
struct Frame {
  Kind kind=Kind::Data;
  uint32_t session=0,sequence=0;
  uint16_t block=0,offset=0,total=0,length=0;
  const uint8_t* data=nullptr;
};
inline void put16(uint8_t* p,uint16_t v) {
  p[0]=static_cast<uint8_t>(v);p[1]=static_cast<uint8_t>(v>>8);
}
inline void put32(uint8_t* p,uint32_t v) {
  put16(p,static_cast<uint16_t>(v));put16(p+2,static_cast<uint16_t>(v>>16));
}
inline uint16_t get16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]|static_cast<uint16_t>(p[1])<<8);
}
inline uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(get16(p))|
         static_cast<uint32_t>(get16(p+2))<<16;
}
inline uint32_t crc32(const uint8_t* bytes,size_t n) {
  uint32_t crc=0xFFFFFFFFu;
  for(size_t i=0;i<n;++i) {
    crc^=bytes[i];
    for(unsigned bit=0;bit<8;++bit)
      crc=(crc>>1)^((crc&1u)?0xEDB88320u:0u);
  }
  return ~crc;
}
inline bool valid(const Frame& f) {
  if(!f.session || f.block>4096 || f.offset>kMaxBlockBytes ||
     f.total>kMaxBlockBytes)return false;
  if(f.kind==Kind::Data)
    return f.data && f.length>0 && f.length<=kMaxPayload &&
           f.total>0 && static_cast<uint32_t>(f.offset)+f.length<=f.total;
  if(f.kind==Kind::End)
    return f.length==0 && !f.offset && !f.total && !f.data;
  if(f.kind==Kind::Ack)
    return f.length==0 && !f.total && !f.data;
  return false;
}
inline size_t encode(const Frame& f,uint8_t* out,size_t capacity) {
  if(!out || !valid(f) || capacity<kHeader+kTrailer+f.length)return 0;
  out[0]='M';out[1]='5';out[2]='S';out[3]='2';
  out[4]=kVersion;out[5]=static_cast<uint8_t>(f.kind);out[6]=out[7]=0;
  put32(out+8,f.session);put32(out+12,f.sequence);
  put16(out+16,f.block);put16(out+18,f.offset);
  put16(out+20,f.total);put16(out+22,f.length);
  for(size_t i=0;i<f.length;++i)out[kHeader+i]=f.data[i];
  const size_t written=kHeader+f.length+kTrailer;
  put32(out+written-kTrailer,crc32(out,written-kTrailer));
  return written;
}
inline bool decode(const uint8_t* in,size_t n,Frame& f) {
  if(!in || n<kHeader+kTrailer || n>kMaxNotification ||
     in[0]!='M'||in[1]!='5'||in[2]!='S'||in[3]!='2'||
     in[4]!=kVersion || in[6] || in[7])return false;
  Frame parsed{};
  parsed.kind=static_cast<Kind>(in[5]);
  parsed.session=get32(in+8);parsed.sequence=get32(in+12);
  parsed.block=get16(in+16);parsed.offset=get16(in+18);
  parsed.total=get16(in+20);parsed.length=get16(in+22);
  if(n!=kHeader+parsed.length+kTrailer ||
     get32(in+n-kTrailer)!=crc32(in,n-kTrailer))return false;
  parsed.data=parsed.length?in+kHeader:nullptr;
  if(!valid(parsed))return false;
  f=parsed;return true;
}
// Advance only after the exact received bytes have been durably verified
// against storage. Never ACK a BLE enqueue or unverified SD write.
class VerifiedCursor {
 public:
  enum class Check:uint8_t {Next,Duplicate,Gap,Invalid};
  bool reset(uint32_t session,uint32_t sequence=0,
             uint16_t block=0,uint16_t offset=0,uint16_t total=0) {
    if(!session || block>4096 || total>kMaxBlockBytes ||
       ((offset==0)!=(total==0)) || (offset && offset>=total))return false;
    session_=session;sequence_=sequence;block_=block;offset_=offset;
    total_=total;complete_=false;return true;
  }
  Check check(const Frame& f)const {
    if(!valid(f) || !session_ || f.session!=session_ ||
       f.kind==Kind::Ack || complete_)return Check::Invalid;
    if(f.sequence<sequence_)return Check::Duplicate;
    if(f.sequence>sequence_)return Check::Gap;
    if(f.block!=block_ || f.offset!=offset_)return Check::Gap;
    if(f.kind==Kind::End)return offset_==0?Check::Next:Check::Invalid;
    if(offset_ && f.total!=total_)return Check::Invalid;
    return Check::Next;
  }
  bool verified(const Frame& f) {
    if(check(f)!=Check::Next || sequence_==UINT32_MAX)return false;
    if(f.kind==Kind::End)complete_=true;
    else {
      if(!offset_)total_=f.total;
      offset_=static_cast<uint16_t>(offset_+f.length);
      if(offset_==total_){++block_;offset_=0;total_=0;}
    }
    ++sequence_;return true;
  }
  Frame ack()const {
    Frame a{};a.kind=Kind::Ack;a.session=session_;a.sequence=sequence_;
    a.block=block_;a.offset=offset_;return a;
  }
  uint32_t sequence()const{return sequence_;}
  uint16_t block()const{return block_;}
  uint16_t offset()const{return offset_;}
  bool complete()const{return complete_;}
 private:
  uint32_t session_=0,sequence_=0;
  uint16_t block_=0,offset_=0,total_=0;
  bool complete_=false;
};
// Bounded go-back-N window. On timeout rewind and regenerate the
// unacknowledged block/offset from the immutable flash journal.
class WindowGate {
 public:
  // The sender can accept only a verified receiving cursor, never an ACK
  // selected by sequence alone. Each of 16 in-flight frames records its
  // expected post-write block/offset checkpoint.
  bool reset(uint32_t session,uint32_t sequence=0,
             uint16_t block=0,uint16_t offset=0,uint16_t total=0) {
    if(!session || block>4096 || total>kMaxBlockBytes ||
       ((offset==0)!=(total==0)) ||
       (offset && offset>=total) || sequence==UINT32_MAX)return false;
    session_=session;acked_=sent_=sequence;
    durable_={block,offset,total};
    pending_=durable_;
    ended_=false;end_sequence_=0;
    return true;
  }
  bool maySend()const {
    return session_ && !ended_ && sent_!=UINT32_MAX &&
           sent_-acked_<kWindowFrames;
  }
  bool sent(const Frame& frame) {
    if(!maySend() || !valid(frame) || frame.session!=session_ ||
       frame.sequence!=sent_ || frame.kind==Kind::Ack ||
       frame.block!=pending_.block || frame.offset!=pending_.offset)return false;
    if(frame.kind==Kind::Data) {
      if(pending_.offset && frame.total!=pending_.total)return false;
      if(!pending_.offset)pending_.total=frame.total;
      const uint32_t advanced=static_cast<uint32_t>(pending_.offset)+frame.length;
      if(advanced>pending_.total)return false;
      pending_.offset=static_cast<uint16_t>(advanced);
      if(advanced==pending_.total) {
        if(pending_.block==4096)return false;
        ++pending_.block;pending_.offset=pending_.total=0;
      }
    } else if(frame.kind==Kind::End) {
      if(pending_.offset)return false;
      ended_=true;end_sequence_=sent_+1;
    } else return false;
    checkpoints_[sent_%kWindowFrames]=pending_;
    ++sent_;
    return true;
  }
  bool acknowledge(const Frame& ack) {
    if(ack.kind!=Kind::Ack || !valid(ack) ||
       !session_ || ack.session!=session_ ||
       ack.sequence<acked_ || ack.sequence>sent_)return false;
    const Position expected=ack.sequence==acked_ ? durable_ :
        checkpoints_[(ack.sequence-1)%kWindowFrames];
    if(ack.block!=expected.block || ack.offset!=expected.offset)return false;
    acked_=ack.sequence;durable_=expected;
    return true;
  }
  void rewindUnacked() {
    sent_=acked_;pending_=durable_;
    // Re-send END only if it has not been durably ACKed.
    ended_=end_sequence_ && acked_>=end_sequence_;
    if(!ended_)end_sequence_=0;
  }
  uint32_t nextToSend()const{return sent_;}
  uint32_t durableNext()const{return acked_;}
  uint32_t outstanding()const{return sent_-acked_;}
  uint16_t durableBlock()const{return durable_.block;}
  uint16_t durableOffset()const{return durable_.offset;}
  uint16_t nextBlock()const{return pending_.block;}
  uint16_t nextOffset()const{return pending_.offset;}
  bool completeAcked()const{return ended_ && acked_>=end_sequence_;}
 private:
  struct Position {
    uint16_t block=0,offset=0,total=0;
  };
  uint32_t session_=0,acked_=0,sent_=0,end_sequence_=0;
  bool ended_=false;
  Position durable_{},pending_{};
  Position checkpoints_[kWindowFrames]{};
};
}
