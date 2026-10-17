#pragma once
// Deterministic, bounded, reversible CAN RAW block codec. No heap/Arduino/RTOS.
// Both encoder and decoder store every sample's arrival-order sequence and
// observed microsecond timestamp. This is NOT a lossy RPM run compressor.
#include "raw_can_capture.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace m5can {

class RawCanBlockCodec {
 public:
  static constexpr size_t kHeader = 32;
  static constexpr size_t kPayload = 3584;
  static constexpr size_t kMaxBytes = kHeader + kPayload;
  static constexpr uint8_t kVersion = 1;

  RawCanBlockCodec() { reset(); }
  void reset() {
    std::memset(buffer_,0,kHeader);
    length_=kHeader; count_=0; previous_=RawCanFrame{};
    first_sequence_=0; first_timestamp_=0;
  }
  uint16_t count() const { return count_; }
  size_t size() const { return length_; }
  const uint8_t* data() const { return buffer_; }
  uint32_t firstSequence() const { return first_sequence_; }

  bool append(const RawCanFrame& frame) {
    if(frame.dlc>8 || frame.flags>7 ||
       static_cast<uint8_t>(frame.direction)>2 ||
       count_==UINT16_MAX)return false;
    if(count_ && (frame.sequence<=previous_.sequence ||
                  frame.timestamp_us<previous_.timestamp_us))return false;
    uint8_t record[32]{};
    size_t n=0;
    const uint32_t sequence_base=count_?previous_.sequence:frame.sequence;
    const uint64_t time_base=count_?previous_.timestamp_us:frame.timestamp_us;
    if(!putVar(frame.sequence-sequence_base,record,sizeof(record),n) ||
       !putVar(frame.timestamp_us-time_base,record,sizeof(record),n))return false;
    const bool same_id=count_ && previous_.identifier==frame.identifier;
    const bool same_payload=count_ && previous_.dlc==frame.dlc &&
        !std::memcmp(previous_.data,frame.data,frame.dlc);
    const uint8_t metadata=static_cast<uint8_t>(
        static_cast<uint8_t>(frame.direction) |
        (static_cast<uint8_t>(frame.flags)<<2) |
        (same_id?0x20:0) | (same_payload?0x40:0));
    if(n+2>sizeof(record))return false;
    record[n++]=metadata;
    record[n++]=frame.dlc;
    if(!same_id && !putVar(frame.identifier,record,sizeof(record),n))return false;
    if(!same_payload) {
      if(n+frame.dlc>sizeof(record))return false;
      std::memcpy(record+n,frame.data,frame.dlc); n+=frame.dlc;
    }
    if(length_+n>kMaxBytes)return false;
    if(!count_) {
      first_sequence_=frame.sequence;
      first_timestamp_=frame.timestamp_us;
    }
    std::memcpy(buffer_+length_,record,n);
    length_+=n; previous_=frame; ++count_;
    return true;
  }

  // Finalize a frozen block. Caller must supply separate sector metadata
  // (session ID, physical flash index, durable CRC/commit-last) externally.
  // A finalized codec block is immutable until reset().
  bool finalize() {
    if(!count_)return false;
    buffer_[0]='M';buffer_[1]='C';buffer_[2]='A';buffer_[3]='N';
    buffer_[4]=kVersion;buffer_[5]=0;
    put16(buffer_+6,count_);
    put32(buffer_+8,first_sequence_);
    put64(buffer_+12,first_timestamp_);
    put32(buffer_+20,static_cast<uint32_t>(length_-kHeader));
    put32(buffer_+24,0);
    put32(buffer_+28,0);
    uint32_t crc=0xFFFFFFFFU;
    crc=crcUpdate(crc,buffer_,24);
    crc=crcUpdate(crc,buffer_+kHeader,length_-kHeader);
    put32(buffer_+24,~crc);
    return true;
  }

  static bool decode(const uint8_t* bytes,size_t length,
                     RawCanFrame* out,size_t capacity,uint16_t& decoded) {
    decoded=0;
    if(!bytes || !out || length<kHeader ||
       bytes[0]!='M'||bytes[1]!='C'||bytes[2]!='A'||bytes[3]!='N' ||
       bytes[4]!=kVersion||bytes[5]!=0)return false;
    const uint16_t wanted=get16(bytes+6);
    const uint32_t first_seq=get32(bytes+8);
    const uint64_t first_time=get64(bytes+12);
    const uint32_t encoded=get32(bytes+20);
    if(!wanted||wanted>capacity||encoded>kPayload ||
       static_cast<size_t>(encoded)+kHeader!=length ||
       get32(bytes+28)!=0)return false;
    uint32_t crc=0xFFFFFFFFU;
    crc=crcUpdate(crc,bytes,24);
    crc=crcUpdate(crc,bytes+kHeader,encoded);
    if((~crc)!=get32(bytes+24))return false;
    size_t offset=kHeader;
    RawCanFrame previous{};
    for(uint16_t i=0;i<wanted;++i) {
      uint64_t seq_delta=0,time_delta=0;
      if(!getVar(bytes,length,offset,seq_delta) ||
         !getVar(bytes,length,offset,time_delta) ||
         offset+2>length)return false;
      const uint8_t metadata=bytes[offset++];
      const uint8_t dlc=bytes[offset++];
      if((metadata&0x80U) || (metadata&3U)>2 || dlc>8)return false;
      if(i==0 && (seq_delta || time_delta))return false;
      if(seq_delta>UINT32_MAX || (i && (!seq_delta ||
          seq_delta>UINT32_MAX-previous.sequence)))return false;
      if(time_delta>UINT64_MAX-(i?previous.timestamp_us:first_time))
        return false;
      RawCanFrame f{};
      f.sequence=i?previous.sequence+static_cast<uint32_t>(seq_delta):first_seq;
      f.timestamp_us=(i?previous.timestamp_us:first_time)+time_delta;
      f.dlc=dlc;
      f.flags=static_cast<uint8_t>((metadata>>2)&7U);
      f.direction=static_cast<RawCanDirection>(metadata&3U);
      if(metadata&0x20U) {
        if(!i)return false;
        f.identifier=previous.identifier;
      } else {
        uint64_t identifier=0;
        if(!getVar(bytes,length,offset,identifier) ||
           identifier>0x1FFFFFFFU)return false;
        f.identifier=static_cast<uint32_t>(identifier);
      }
      if(metadata&0x40U) {
        if(!i||previous.dlc!=dlc)return false;
        std::memcpy(f.data,previous.data,dlc);
      } else {
        if(offset+dlc>length)return false;
        std::memcpy(f.data,bytes+offset,dlc);
        offset+=dlc;
      }
      if((f.flags&1U)?f.identifier>0x1FFFFFFFU:f.identifier>0x7FFU)
        return false;
      out[i]=f;previous=f;
    }
    if(offset!=length)return false;
    decoded=wanted;return true;
  }

 private:
  static void put16(uint8_t* p,uint16_t v){
    p[0]=static_cast<uint8_t>(v);p[1]=static_cast<uint8_t>(v>>8);
  }
  static void put32(uint8_t* p,uint32_t v){
    for(unsigned i=0;i<4;++i)p[i]=static_cast<uint8_t>(v>>(8*i));
  }
  static void put64(uint8_t* p,uint64_t v){
    for(unsigned i=0;i<8;++i)p[i]=static_cast<uint8_t>(v>>(8*i));
  }
  static uint16_t get16(const uint8_t* p){
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(p[1])<<8;
  }
  static uint32_t get32(const uint8_t* p){
    uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(p[i])<<(8*i);
    return v;
  }
  static uint64_t get64(const uint8_t* p){
    uint64_t v=0;for(unsigned i=0;i<8;++i)v|=uint64_t(p[i])<<(8*i);
    return v;
  }
  static bool putVar(uint64_t v,uint8_t* output,size_t cap,size_t& n){
    do{
      if(n>=cap)return false;
      uint8_t b=static_cast<uint8_t>(v&0x7FU);v>>=7;
      if(v)b|=0x80U;
      output[n++]=b;
    }while(v);
    return true;
  }
  static bool getVar(const uint8_t* data,size_t length,size_t& n,uint64_t& v){
    v=0;
    for(unsigned i=0;i<10;++i){
      if(n>=length)return false;
      const uint8_t b=data[n++];
      if(i==9 && (b&0xFEU))return false;
      v|=uint64_t(b&0x7FU)<<(i*7);
      if(!(b&0x80U))return true;
    }
    return false;
  }
  static uint32_t crcUpdate(uint32_t crc,const uint8_t* p,size_t n) {
    while(n--) {
      crc^=*p++;
      for(unsigned i=0;i<8;++i)crc=(crc>>1) ^
          ((crc&1U)?0xEDB88320U:0U);
    }
    return crc;
  }

  uint8_t buffer_[kMaxBytes]{};
  size_t length_=kHeader;
  uint16_t count_=0;
  uint32_t first_sequence_=0;
  uint64_t first_timestamp_=0;
  RawCanFrame previous_{};
};

} // namespace m5can
