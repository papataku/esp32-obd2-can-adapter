#pragma once
// Narrow bounded ASCII-over-ELM protocol for M5CAN RAW export v1.
// Each request names an immutable committed block and a byte offset.
// Replies are generated on the Arduino/flash owner, not inside a BLE callback.
// Standalone and host-testable: no Arduino headers, heap, or CAN dependencies.
#include <cstddef>
#include <cstdint>

namespace m5can {
namespace raw_ble {

constexpr uint32_t kChunkBytes = 48;
constexpr const char* kReadPrefix = "ATM5LOGREAD,";

inline bool parseDecimal(const char*& p,uint32_t& result) {
  if(*p<'0'||*p>'9')return false;
  uint32_t value=0;
  do {
    const uint32_t digit=static_cast<uint32_t>(*p-'0');
    if(value>(UINT32_MAX-digit)/10)return false;
    value=value*10+digit;
    ++p;
  }while(*p>='0'&&*p<='9');
  result=value;
  return true;
}
inline bool parseReadArgs(const char* p,uint32_t& block,uint32_t& offset) {
  if(!p || !parseDecimal(p,block) || *p++!=',' ||
     !parseDecimal(p,offset) || *p!='\0')return false;
  return true;
}
inline uint32_t crc32(const uint8_t* bytes,size_t length) {
  uint32_t crc=0xFFFFFFFFU;
  for(size_t n=0;n<length;++n) {
    crc^=bytes[n];
    for(unsigned bit=0;bit<8;++bit)
      crc=(crc>>1)^((crc&1U)?0xEDB88320U:0);
  }
  return ~crc;
}
inline bool hexEncode(const uint8_t* input,size_t n,char* out,size_t cap) {
  constexpr char kHex[]="0123456789ABCDEF";
  if(!out || (n && !input) || n>(cap-1)/2 || !cap)return false;
  for(size_t i=0;i<n;++i) {
    out[2*i]=kHex[input[i]>>4];
    out[2*i+1]=kHex[input[i]&0x0FU];
  }
  out[n*2]='\0';
  return true;
}
} // namespace raw_ble
} // namespace m5can
