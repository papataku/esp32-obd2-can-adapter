#pragma once
// Standalone opt-in BLE LOG V2 service; NEVER reuse the ASCII ELM UART.
// A START is a 20-byte GATT WRITE message (CRC-protected). All ACK
// messages use the existing 28-byte M5S2 Kind::Ack frame encoding.
// Authorization (encrypted bonded connection + selected M5CAN device) and
// subscribed peer generation must be checked by the transport, NOT here.
#include "m5can_log_stream_v2.h"
namespace m5can_log_stream_v2 {
static constexpr const char* kGattService="b8596400-a2d7-4c26-9918-6a2d0b88fa00";
static constexpr const char* kGattControl="b8596401-a2d7-4c26-9918-6a2d0b88fa00";
static constexpr const char* kGattData="b8596402-a2d7-4c26-9918-6a2d0b88fa00";
constexpr size_t kStartBytes=20;
struct StartRequest {
  uint32_t session=0;
  uint16_t blocks=0;
};
inline size_t encodeStart(const StartRequest& request,uint8_t* out,size_t cap) {
  if(!out || cap<kStartBytes || !request.session ||
     request.blocks==0 || request.blocks>4096)return 0;
  out[0]='M';out[1]='5';out[2]='C';out[3]='2';
  out[4]=kVersion;out[5]=1;out[6]=out[7]=0;
  put32(out+8,request.session);put16(out+12,request.blocks);
  out[14]=out[15]=0;
  put32(out+16,crc32(out,16));
  return kStartBytes;
}
inline bool decodeStart(const uint8_t* in,size_t n,StartRequest& out) {
  if(!in || n!=kStartBytes || in[0]!='M'||in[1]!='5'||
     in[2]!='C'||in[3]!='2'||in[4]!=kVersion||in[5]!=1||
     in[6]||in[7]||in[14]||in[15]||
     crc32(in,16)!=get32(in+16))return false;
  StartRequest tmp{};
  tmp.session=get32(in+8);tmp.blocks=get16(in+12);
  if(!tmp.session || !tmp.blocks || tmp.blocks>4096)return false;
  out=tmp;return true;
}
enum class ControlKind:uint8_t {Invalid,Start,Ack};
struct Control {
  ControlKind kind=ControlKind::Invalid;
  StartRequest start{};
  Frame ack{};
};
inline bool decodeControl(const uint8_t* in,size_t n,Control& out) {
  Control candidate{};
  if(n==kStartBytes && decodeStart(in,n,candidate.start))
    candidate.kind=ControlKind::Start;
  else if(n==kHeader+kTrailer && decode(in,n,candidate.ack) &&
          candidate.ack.kind==Kind::Ack)
    candidate.kind=ControlKind::Ack;
  else return false;
  out=candidate;return true;
}
}
